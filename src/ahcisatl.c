#include "ahcint.h"

/* ahcisatl.c - SCSI-to-ATA translation, ATAPI pass-through, command engine */

/* ------------------------------------------------------------------ */
/* 64-bit block number helpers (two ULONGs, no __int64 needed)         */
/* ------------------------------------------------------------------ */

static AHCI_LBA
AhciLbaFromBigEndian(IN PUCHAR Bytes, IN ULONG Count)
{
    AHCI_LBA lba;
    ULONG i;

    lba.Low = 0;
    lba.High = 0;
    for (i = 0; i < Count; i++) {
        lba.High = (lba.High << 8) | (lba.Low >> 24);
        lba.Low = (lba.Low << 8) | Bytes[i];
    }
    return lba;
}

static ULONG
AhciBigEndian32(IN PUCHAR Bytes)
{
    return ((ULONG)Bytes[0] << 24) | ((ULONG)Bytes[1] << 16) |
           ((ULONG)Bytes[2] << 8)  |  (ULONG)Bytes[3];
}

static VOID
AhciPutBigEndian32(OUT PUCHAR Bytes, IN ULONG Value)
{
    Bytes[0] = (UCHAR)(Value >> 24);
    Bytes[1] = (UCHAR)(Value >> 16);
    Bytes[2] = (UCHAR)(Value >> 8);
    Bytes[3] = (UCHAR)Value;
}

/* Last addressable LBA (capacity - 1); 0 for an empty/unknown device. */
static AHCI_LBA
AhciLastLba(IN PAHCI_PORT_INFO Pi)
{
    AHCI_LBA last = Pi->TotalSectors;

    if (last.Low == 0 && last.High == 0) return last;
    if (last.Low == 0) last.High--;
    last.Low--;
    return last;
}

/* Lba + Count <= capacity ? */
static BOOLEAN
AhciLbaRangeValid(IN PAHCI_PORT_INFO Pi, IN AHCI_LBA Lba, IN ULONG Count)
{
    ULONG endLow, endHigh;

    endLow = Lba.Low + Count;
    endHigh = Lba.High;
    if (endLow < Lba.Low) {
        if (endHigh == 0xFFFFFFFF) return FALSE;
        endHigh++;
    }

    if (endHigh != Pi->TotalSectors.High) return (BOOLEAN)(endHigh < Pi->TotalSectors.High);
    return (BOOLEAN)(endLow <= Pi->TotalSectors.Low);
}

/* 28-bit command usable? Same rule as Linux lba_28_ok(): the transfer
   must end below 2^28 and move at most 256 sectors. */
static BOOLEAN
AhciLbaFits28(IN AHCI_LBA Lba, IN ULONG Count)
{
    if (Lba.High != 0 || Lba.Low >= ATA_LBA28_LIMIT) return FALSE;
    if (Count > ATA_LBA28_MAX_SECTORS) return FALSE;
    return (BOOLEAN)((ATA_LBA28_LIMIT - Lba.Low) > Count);
}

/* ------------------------------------------------------------------ */
/* IDENTIFY DEVICE                                                     */
/* ------------------------------------------------------------------ */

/* Called once per port after IDENTIFY. Decides whether the disk takes
   48-bit (EXT) commands and what its capacity is. */
VOID
AhciParseIdentify(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port)
{
    PAHCI_PORT_INFO pi;
    PUSHORT id;
    AHCI_LBA lba48;

    pi = &HwInit->Ports[Port];
    id = (PUSHORT)pi->IdentifyData;

    pi->LbaSupported = FALSE;
    pi->Lba48 = FALSE;
    pi->TotalSectors.Low = 0;
    pi->TotalSectors.High = 0;
    pi->SectorSize = AHCI_SECTOR_SIZE;
    pi->PhysExponent = 0;

    if (!pi->IdentifyValid || pi->IsAtapi) return;

    /* Word 106: logical sector size (words 117-118) and logical sectors per
       physical sector; all LBAs and counts are in logical sectors. */
    if ((id[IDW_SECTOR_SIZE_INFO] & IDW106_VALID_MASK) == IDW106_VALID) {
        if (id[IDW_SECTOR_SIZE_INFO] & IDW106_LONG_LOGICAL) {
            ULONG bytes = ((ULONG)id[IDW_LOGICAL_SECTOR_WORDS] |
                           ((ULONG)id[IDW_LOGICAL_SECTOR_WORDS + 1] << 16)) * 2;
            if (bytes >= AHCI_SECTOR_SIZE && bytes <= AHCI_MAX_SECTOR_SIZE &&
                (bytes & (bytes - 1)) == 0) {
                pi->SectorSize = bytes;
            } else {
                AHCI_DBG((AHCI_PFX "Port %lu: logical sector of %lu bytes not supported\n", Port, bytes));
                pi->IdentifyValid = FALSE;
                return;
            }
        }
        if (id[IDW_SECTOR_SIZE_INFO] & IDW106_MULTI_LOGICAL) {
            pi->PhysExponent = (UCHAR)(id[IDW_SECTOR_SIZE_INFO] & IDW106_PHYS_EXP_MASK);
        }
    }

    pi->LbaSupported = (BOOLEAN)((id[IDW_CAPABILITIES] & IDW49_LBA_SUPPORTED) != 0);

    /* LBA48: word 83 valid (15:14 = 01) with bit 10, and words 100-103 != 0 */
    lba48.Low = (ULONG)id[IDW_LBA48_SECTORS] | ((ULONG)id[IDW_LBA48_SECTORS + 1] << 16);
    lba48.High = (ULONG)id[IDW_LBA48_SECTORS + 2] | ((ULONG)id[IDW_LBA48_SECTORS + 3] << 16);

    if (pi->LbaSupported &&
        (id[IDW_CMDSET_SUPPORTED_2] & IDW83_VALID_MASK) == IDW83_VALID &&
        (id[IDW_CMDSET_SUPPORTED_2] & IDW83_LBA48) &&
        (lba48.Low != 0 || lba48.High != 0)) {
        pi->Lba48 = TRUE;
        pi->TotalSectors = lba48;
    } else if (pi->LbaSupported) {
        pi->TotalSectors.Low = (ULONG)id[IDW_LBA28_SECTORS] | ((ULONG)id[IDW_LBA28_SECTORS + 1] << 16);
    }

    AHCI_DBG((AHCI_PFX "Port %lu: LBA=%s LBA48=%s (W83=%04lX W86=%04lX) sectors=%08lX:%08lX x %lu bytes, %lu per physical\n",
              Port,
              pi->LbaSupported ? "yes" : "no",
              pi->Lba48 ? "yes" : "no",
              (ULONG)id[IDW_CMDSET_SUPPORTED_2], (ULONG)id[IDW_CMDSET_ENABLED_2],
              pi->TotalSectors.High, pi->TotalSectors.Low,
              pi->SectorSize, 1UL << pi->PhysExponent));

    if (!pi->LbaSupported) {
        /* SATA mandates LBA; a CHS-only device is not addressable here. */
        AHCI_DBG((AHCI_PFX "Port %lu: device has no LBA support, not usable\n", Port));
    }
}

/* ------------------------------------------------------------------ */
/* Sense data                                                          */
/* ------------------------------------------------------------------ */

static VOID
AhciBuildSense(OUT PSENSE_DATA Sense, IN UCHAR Key, IN UCHAR Asc, IN UCHAR Ascq)
{
    ZeroMemoryBytes(Sense, sizeof(SENSE_DATA));
    Sense->ErrorCode = 0x70;
    Sense->Valid = (Key != SCSI_SENSE_NO_SENSE) ? 1 : 0;
    Sense->SenseKey = Key;
    Sense->AdditionalSenseLength = sizeof(SENSE_DATA) - 8;
    Sense->AdditionalSenseCode = Asc;
    Sense->AdditionalSenseCodeQualifier = Ascq;
}

#if AHCI_AUTOSENSE
static BOOLEAN
AhciAutoSenseWanted(IN PSCSI_REQUEST_BLOCK Srb)
{
    return (BOOLEAN)(Srb->SenseInfoBuffer != NULL &&
                     Srb->SenseInfoBufferLength != 0 &&
                     !(Srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE));
}

static VOID
AhciSetAutoSense(IN PSCSI_REQUEST_BLOCK Srb, IN PVOID Sense, IN ULONG Length)
{
    ULONG n = Length;

    if (n > Srb->SenseInfoBufferLength) n = Srb->SenseInfoBufferLength;
    ZeroMemoryBytes(Srb->SenseInfoBuffer, Srb->SenseInfoBufferLength);
    CopyMemoryBytes(Srb->SenseInfoBuffer, Sense, n);
    Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
}
#endif

/* CHECK CONDITION with sense: auto-sense, or kept for the next REQUEST SENSE */
static VOID
AhciFailWithSense(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN UCHAR Key,
    IN UCHAR Asc,
    IN UCHAR Ascq
)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];

    Srb->SrbStatus = SRB_STATUS_ERROR;
    Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;

    if (Key == SCSI_SENSE_NO_SENSE) return;     /* nothing specific to report */

#if AHCI_AUTOSENSE
    if (AhciAutoSenseWanted(Srb)) {
        SENSE_DATA sense;
        AhciBuildSense(&sense, Key, Asc, Ascq);
        AhciSetAutoSense(Srb, &sense, sizeof(SENSE_DATA));
        return;
    }
#endif

    pi->SensePending = TRUE;
    pi->SenseKey = Key;
    pi->SenseAsc = Asc;
    pi->SenseAscq = Ascq;
}

/* ATA Error register -> sense (subset of the SAT / libata mapping) */
static VOID
AhciAtaErrorToSense(IN ULONG Tfd, OUT PUCHAR Key, OUT PUCHAR Asc, OUT PUCHAR Ascq)
{
    UCHAR err = (UCHAR)(Tfd >> TFD_ERR_SHIFT);

    *Key = SCSI_SENSE_NO_SENSE;
    *Asc = 0;
    *Ascq = 0;

    if (!(Tfd & TFD_STS_ERR)) return;           /* HBA error / timeout */

    if (err & (IDE_ERROR_MEDIA_CHANGE | IDE_ERROR_MEDIA_CHANGE_REQ)) {
        *Key = SCSI_SENSE_UNIT_ATTENTION;  *Asc = AHCI_ASC_MEDIUM_CHANGED;
    } else if (err & IDE_ERROR_UNCORRECTABLE) {
        *Key = SCSI_SENSE_MEDIUM_ERROR;    *Asc = AHCI_ASC_UNRECOVERED_READ; *Ascq = 0x04;
    } else if (err & IDE_ERROR_ID_NOT_FOUND) {
        *Key = SCSI_SENSE_ABORTED_COMMAND; *Asc = 0x14;
    } else if (err & IDE_ERROR_ABORT) {
        *Key = SCSI_SENSE_ABORTED_COMMAND;
    } else {
        *Key = SCSI_SENSE_HARDWARE_ERROR;
    }
}

/* ------------------------------------------------------------------ */
/* Command engine (Fast Polling)                                       */
/* ------------------------------------------------------------------ */

#define AHCI_CMD_OK             0
#define AHCI_CMD_DEVICE_ERROR   1   /* PxTFD.ERR: device rejected/failed it */
#define AHCI_CMD_HBA_ERROR      2   /* fatal PxIS without ERR, or timeout */
#define AHCI_CMD_BAD_BUFFER     3   /* buffer could not be described */

static BOOLEAN
AhciBuildPrdt(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PAHCI_PRDT_ENTRY Prdt,
    IN PVOID Buffer,
    IN ULONG Length,
    OUT PULONG Entries
)
{
    PUCHAR virt = (PUCHAR)Buffer;
    ULONG left = Length;
    ULONG index = 0;
    ULONG segLen;
    ULONG toPage;
    SCSI_PHYSICAL_ADDRESS phys;

    while (left > 0 && index < MAX_PRDT_ENTRIES) {
        phys = ScsiPortGetPhysicalAddress(HwInit, Srb, virt, &segLen);
        if (phys.LowPart == 0 && phys.HighPart == 0) return FALSE;

        if (segLen > left) segLen = left;
        toPage = 4096 - (phys.LowPart & 0xFFF);
        if (segLen > toPage) segLen = toPage;

        Prdt[index].DataBaseAddress = phys.LowPart;
        Prdt[index].DataBaseAddressUpper = (ULONG)phys.HighPart;
        Prdt[index].Reserved = 0;
        Prdt[index].ByteCountInterrupt = (segLen - 1) & 0x003FFFFF;

        virt += segLen;
        left -= segLen;
        index++;
    }

    if (left > 0) return FALSE;
    *Entries = index;
    return TRUE;
}

/* Issue one command on slot 0 and poll it to completion. */
static ULONG
AhciRunCommand(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PATA_REQUEST Req
)
{
    PAHCI_PORT_INFO pi;
    PUCHAR portBase;
    PAHCI_COMMAND_HEADER cmdHeader;
    PFIS_REG_H2D fis;
    PUCHAR acmd;
    PAHCI_PRDT_ENTRY prdt;
    ULONG prdtEntries;
    ULONG elapsed;
    ULONG timeoutUsec;
    ULONG ci, portIs, tfd;
    ULONG result;
    ULONG i;

    pi = &HwInit->Ports[Port];
    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    cmdHeader = &pi->CommandList[0];
    fis = (PFIS_REG_H2D)pi->CommandTable;
    acmd = pi->CommandTable + AHCI_CMDTBL_ACMD_OFFSET;
    prdt = (PAHCI_PRDT_ENTRY)(pi->CommandTable + AHCI_CMDTBL_PRDT_OFFSET);

    /* Data */
    prdtEntries = 0;
    if (Req->DataBufferLen > 0) {
        if (Req->UseBounce) {
            prdt[0].DataBaseAddress = pi->BouncePhysical;
            prdt[0].DataBaseAddressUpper = pi->BouncePhysicalUpper;
            prdt[0].Reserved = 0;
            prdt[0].ByteCountInterrupt = Req->DataBufferLen - 1;
            prdtEntries = 1;
        } else if (!AhciBuildPrdt(HwInit, Srb, prdt, Req->DataBuffer, Req->DataBufferLen, &prdtEntries)) {
            AHCI_DBG((AHCI_PFX "Port %lu: cannot build PRDT for %lu bytes\n", Port, Req->DataBufferLen));
            return AHCI_CMD_BAD_BUFFER;
        }
    }

    cmdHeader->Flags = (USHORT)(AHCI_CMDHDR_CFL_H2D |
                                (Req->IsAtapi ? AHCI_CMDHDR_ATAPI : 0) |
                                ((Req->IsWrite && Req->DataBufferLen > 0) ? AHCI_CMDHDR_WRITE : 0));
    cmdHeader->PrdtLength = (USHORT)prdtEntries;
    cmdHeader->PrdByteCount = 0;
    cmdHeader->CommandTableBase = pi->CommandTablePhysical;
    cmdHeader->CommandTableBaseUpper = pi->CommandTablePhysicalUpper;

    /* FIS */
    ZeroMemoryBytes(fis, sizeof(FIS_REG_H2D));
    fis->FisType = FIS_TYPE_REG_H2D;
    fis->PmPortControl = FIS_H2D_COMMAND;

    if (Req->IsAtapi) {
        ULONG byteCount = (Req->DataBufferLen > 0xFFFE) ? 0xFFFE : Req->DataBufferLen;

        ZeroMemoryBytes(acmd, 16);
        for (i = 0; i < Req->CdbLength && i < 16; i++) acmd[i] = Req->Cdb[i];

        fis->Command = IDE_COMMAND_PACKET;
#ifdef AHCI_NT4
        /* NT4/9x: DMA bit on every packet */
        fis->FeaturesLow = 0x01;
#else
        fis->FeaturesLow = (Req->DataBufferLen > 0) ? 0x01 : 0x00;      /* DMA */
#endif
        fis->Lba1 = (UCHAR)(byteCount & 0xFF);
        fis->Lba2 = (UCHAR)((byteCount >> 8) & 0xFF);
    } else {
        fis->Command = Req->Command;
        fis->Lba0 = (UCHAR)(Req->Lba.Low);
        fis->Lba1 = (UCHAR)(Req->Lba.Low >> 8);
        fis->Lba2 = (UCHAR)(Req->Lba.Low >> 16);
        if (Req->Is48Bit) {
            fis->Device = ATA_DEV_LBA;
            fis->Lba3 = (UCHAR)(Req->Lba.Low >> 24);
            fis->Lba4 = (UCHAR)(Req->Lba.High);
            fis->Lba5 = (UCHAR)(Req->Lba.High >> 8);
            /* 65536 sectors is encoded as 0 */
            fis->SectorCountLow = (UCHAR)(Req->SectorCount);
            fis->SectorCountHigh = (UCHAR)(Req->SectorCount >> 8);
        } else {
            fis->Device = (UCHAR)(ATA_DEV_LBA | ((Req->Lba.Low >> 24) & 0x0F));
            /* 256 sectors is encoded as 0 */
            fis->SectorCountLow = (UCHAR)(Req->SectorCount);
        }
    }

    /* Go */
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IE, 0);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << Port));

    AHCI_WRITE_REG(portBase, AHCI_PORT_CI, 1);

    if (Srb == NULL) {
        timeoutUsec = AHCI_POLL_TIMEOUT_INTERNAL_SEC;
    } else {
        timeoutUsec = Srb->TimeOutValue ? Srb->TimeOutValue : AHCI_POLL_TIMEOUT_DEFAULT_SEC;
        if (timeoutUsec > 1) timeoutUsec--;
        if (timeoutUsec > AHCI_POLL_TIMEOUT_MAX_SEC) timeoutUsec = AHCI_POLL_TIMEOUT_MAX_SEC;
    }
    timeoutUsec *= 1000000UL;

    result = AHCI_CMD_OK;
    elapsed = 0;
    for (;;) {
        ci = AHCI_READ_REG(portBase, AHCI_PORT_CI);
        portIs = AHCI_READ_REG(portBase, AHCI_PORT_IS);
        tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);

        if ((portIs & AHCI_PORT_IS_FATAL) || (tfd & TFD_STS_ERR)) {
            result = (tfd & TFD_STS_ERR) ? AHCI_CMD_DEVICE_ERROR : AHCI_CMD_HBA_ERROR;
            break;
        }
        if (!(ci & 1)) break;
        if (elapsed >= timeoutUsec) {
            result = AHCI_CMD_HBA_ERROR;
            break;
        }
        ScsiPortStallExecution(AHCI_POLL_INTERVAL_USEC);
        elapsed += AHCI_POLL_INTERVAL_USEC;
    }

    if (portIs != 0) AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs);
    if (portIs & AHCI_PORT_IS_FATAL) AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << Port));

    pi->LastTfd = tfd;

    if (result != AHCI_CMD_OK) {
        AHCI_DBG((AHCI_PFX "Port %lu: cmd %02lX %s, TFD=%08lX IS=%08lX CI=%08lX\n",
                  Port,
                  (ULONG)(Req->IsAtapi ? Req->Cdb[0] : Req->Command),
                  (elapsed >= timeoutUsec) ? "timed out" : "failed",
                  tfd, portIs, ci));
        AhciRestartPort(HwInit, portBase);
    }
    return result;
}

/* ATAPI command with data in the port's bounce buffer */
static ULONG
AhciAtapiBounceCommand(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port,
    IN PUCHAR Cdb,
    IN ULONG DataLength
)
{
    ATA_REQUEST req;

    ZeroMemoryBytes(&req, sizeof(req));
    req.IsAtapi = TRUE;
    req.UseBounce = TRUE;
    req.DataBufferLen = DataLength;
    req.Cdb = Cdb;
    req.CdbLength = 12;

    ZeroMemoryBytes(HwInit->Ports[Port].BounceBuffer, AHCI_BOUNCE_SIZE);
    return AhciRunCommand(HwInit, Port, NULL, &req);
}

/* ------------------------------------------------------------------ */
/* Commands answered by the driver itself                              */
/* ------------------------------------------------------------------ */

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
    ULONG outIndex = 0;
    USHORT w;

    for (i = 0; i < NumWords && (outIndex + 1) < OutBufferMax; i++) {
        w = IdentifyWords[StartWord + i];
        OutBuffer[outIndex++] = (UCHAR)(w >> 8);
        OutBuffer[outIndex++] = (UCHAR)w;
    }
    while (outIndex > 0 && (OutBuffer[outIndex - 1] == ' ' || OutBuffer[outIndex - 1] == '\0')) {
        outIndex--;
    }
    while (outIndex < OutBufferMax) {
        OutBuffer[outIndex++] = ' ';
    }
}

static VOID
AhciCopyToSrb(IN PSCSI_REQUEST_BLOCK Srb, IN PVOID Data, IN ULONG Length)
{
    ULONG n = Length;

    if (Srb->DataBuffer == NULL) return;
    if (n > Srb->DataTransferLength) n = Srb->DataTransferLength;
    ZeroMemoryBytes(Srb->DataBuffer, Srb->DataTransferLength);
    CopyMemoryBytes(Srb->DataBuffer, Data, n);
}

/* EVPD: supported-pages list and unit serial number (IDENTIFY 10..19) */
static VOID
AhciHandleInquiryVpd(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    UCHAR page[4 + 20];
    ULONG len;

    ZeroMemoryBytes(page, sizeof(page));
    page[0] = pi->IsAtapi ? READ_ONLY_DIRECT_ACCESS_DEVICE : DIRECT_ACCESS_DEVICE;
    page[1] = Srb->Cdb[2];

    switch (Srb->Cdb[2]) {
    case AHCI_VPD_SUPPORTED_PAGES:
        page[3] = 2;
        page[4] = AHCI_VPD_SUPPORTED_PAGES;
        page[5] = AHCI_VPD_SERIAL_NUMBER;
        len = 4 + 2;
        break;

    case AHCI_VPD_SERIAL_NUMBER:
        page[3] = 20;
        if (pi->IdentifyValid) {
            AhciExtractAtaString((PUSHORT)pi->IdentifyData, IDW_SERIAL, 10, &page[4], 20);
        } else {
            ULONG i;
            for (i = 0; i < 20; i++) page[4 + i] = ' ';
        }
        len = 4 + 20;
        break;

    default:
        AhciFailWithSense(HwInit, Port, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_INVALID_CDB_FIELD, 0);
        return;
    }

    if (len > Srb->Cdb[4]) len = Srb->Cdb[4];
    AhciCopyToSrb(Srb, page, len);
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

static VOID
AhciHandleInquiry(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    PUSHORT id = (PUSHORT)pi->IdentifyData;
    INQUIRYDATA inq;
    PUCHAR v;
    PUCHAR p;
    ULONG i;

    if (Srb->Cdb[1] & 0x01) {
        AhciHandleInquiryVpd(HwInit, Port, Srb);
        return;
    }
    if (Srb->Cdb[2] != 0) {                     /* page code without EVPD */
        AhciFailWithSense(HwInit, Port, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_INVALID_CDB_FIELD, 0);
        return;
    }

    ZeroMemoryBytes(&inq, sizeof(inq));

    inq.DeviceType = pi->IsAtapi ? READ_ONLY_DIRECT_ACCESS_DEVICE : DIRECT_ACCESS_DEVICE;
    inq.RemovableMedia = pi->IsAtapi ? 1 : ((id[IDW_GENERAL_CONFIG] & (1 << 7)) ? 1 : 0);
    /* ATAPI stays unversioned */
    inq.Versions = pi->IsAtapi ? 0 : AHCI_INQUIRY_VERSION_SPC3;
    inq.ResponseDataFormat = 2;
    inq.AdditionalLength = 31;
    inq.CommandQueue = 0;       /* one command at a time */

    if (pi->IdentifyValid) {
        AhciExtractAtaString(id, IDW_MODEL, 4, inq.VendorId, 8);
        AhciExtractAtaString(id, IDW_MODEL + 4, 8, inq.ProductId, 16);
        AhciExtractAtaString(id, IDW_FIRMWARE, 2, inq.ProductRevisionLevel, 4);
    } else {
        v = (PUCHAR)(pi->IsAtapi ? "ATAPI   " : "ATA     ");
        p = (PUCHAR)(pi->IsAtapi ? "SATA CD-ROM     " : "SATA HARDDISK   ");
        for (i = 0; i < 8; i++) inq.VendorId[i] = v[i];
        for (i = 0; i < 16; i++) inq.ProductId[i] = p[i];
        inq.ProductRevisionLevel[0] = '1';
        inq.ProductRevisionLevel[1] = '.';
        inq.ProductRevisionLevel[2] = '0';
        inq.ProductRevisionLevel[3] = '0';
    }

    AhciCopyToSrb(Srb, &inq, sizeof(inq));
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

/* READ CAPACITY(10): last LBA saturates at 0xFFFFFFFF, which tells the
   class driver to use READ CAPACITY(16). */
static VOID
AhciHandleReadCapacity10(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR data[8];
    AHCI_LBA last = AhciLastLba(&HwInit->Ports[Port]);

    AhciPutBigEndian32(&data[0], (last.High != 0) ? 0xFFFFFFFF : last.Low);
    AhciPutBigEndian32(&data[4], HwInit->Ports[Port].SectorSize);

    AhciCopyToSrb(Srb, data, sizeof(data));
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

/* SERVICE ACTION IN(16) / READ CAPACITY(16): full 64-bit last LBA */
static VOID
AhciHandleReadCapacity16(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR data[AHCI_READ_CAPACITY16_DATA_LEN];
    AHCI_LBA last = AhciLastLba(&HwInit->Ports[Port]);
    ULONG allocLen = AhciBigEndian32(&Srb->Cdb[10]);
    ULONG n;

    ZeroMemoryBytes(data, sizeof(data));
    AhciPutBigEndian32(&data[0], last.High);
    AhciPutBigEndian32(&data[4], last.Low);
    AhciPutBigEndian32(&data[8], HwInit->Ports[Port].SectorSize);
    data[13] = (UCHAR)(HwInit->Ports[Port].PhysExponent & 0x0F);

    n = sizeof(data);
    if (n > allocLen) n = allocLen;
    AhciCopyToSrb(Srb, data, n);
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

static VOID
AhciHandleRequestSenseAta(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    SENSE_DATA sense;

    if (pi->SensePending) {
        AhciBuildSense(&sense, pi->SenseKey, pi->SenseAsc, pi->SenseAscq);
        pi->SensePending = FALSE;
    } else {
        AhciBuildSense(&sense, SCSI_SENSE_NO_SENSE, 0, 0);
    }

    AhciCopyToSrb(Srb, &sense, sizeof(sense));
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

/* ATA MODE SENSE: block descriptor and page 04h; other pages header only */
static VOID
AhciHandleModeSenseAta(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    PUSHORT id = (PUSHORT)pi->IdentifyData;
    BOOLEAN ten = (BOOLEAN)(Srb->Cdb[0] == SCSIOP_MODE_SENSE10);
    BOOLEAN dbd = (BOOLEAN)((Srb->Cdb[1] & 0x08) != 0);
    BOOLEAN llba = (BOOLEAN)(ten && (Srb->Cdb[1] & 0x10) != 0);    /* LLBAA */
    UCHAR pageCode = (UCHAR)(Srb->Cdb[2] & 0x3F);
    ULONG allocLen;
    UCHAR buf[64];
    ULONG hdrLen, len, descLen;
    ULONG blocks, cyls, heads, spt;
    PUCHAR p;

    allocLen = ten ? (((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8]) : Srb->Cdb[4];

    ZeroMemoryBytes(buf, sizeof(buf));
    hdrLen = ten ? 8 : 4;
    len = hdrLen;

    descLen = 0;
    if (!dbd && llba) {
        /* Long LBA block descriptor (MODE SENSE(10) only): 64-bit count */
        p = &buf[len];
        AhciPutBigEndian32(&p[0], pi->TotalSectors.High);
        AhciPutBigEndian32(&p[4], pi->TotalSectors.Low);
        AhciPutBigEndian32(&p[12], pi->SectorSize);
        descLen = 16;
        len += 16;
    } else if (!dbd) {
        /* Short block descriptor: 24-bit block count, saturating */
        blocks = (pi->TotalSectors.High != 0 || pi->TotalSectors.Low > 0xFFFFFF)
                     ? 0xFFFFFF : pi->TotalSectors.Low;
        p = &buf[len];
        p[1] = (UCHAR)(blocks >> 16);
        p[2] = (UCHAR)(blocks >> 8);
        p[3] = (UCHAR)blocks;
        p[5] = (UCHAR)(pi->SectorSize >> 16);
        p[6] = (UCHAR)(pi->SectorSize >> 8);
        p[7] = (UCHAR)pi->SectorSize;
        descLen = 8;
        len += 8;
    }

    if (pageCode == 0x04 || pageCode == 0x3F) {
        cyls = heads = spt = 0;
        if (pi->IdentifyValid) {
            cyls = id[IDW_CYLINDERS];
            heads = id[IDW_HEADS];
            spt = id[IDW_SECTORS_PER_TRACK];
        }
        if (cyls == 0 || heads == 0 || spt == 0) {
            ULONG total32 = (pi->TotalSectors.High != 0) ? 0xFFFFFFFF : pi->TotalSectors.Low;
            heads = 255;
            spt = 63;
            cyls = total32 / (heads * spt);
            if (cyls == 0) cyls = 1;
        }
        p = &buf[len];
        p[0] = 0x04;
        p[1] = 0x16;
        p[2] = (UCHAR)(cyls >> 16);
        p[3] = (UCHAR)(cyls >> 8);
        p[4] = (UCHAR)cyls;
        p[5] = (UCHAR)heads;
        p[20] = (UCHAR)(spt >> 8);      /* rotation-rate field */
        p[21] = (UCHAR)spt;
        len += 24;
    }

    if (ten) {
        buf[0] = (UCHAR)((len - 2) >> 8);
        buf[1] = (UCHAR)(len - 2);
        buf[4] = (UCHAR)((descLen == 16) ? 0x01 : 0x00);    /* LONGLBA */
        buf[7] = (UCHAR)descLen;
    } else {
        buf[0] = (UCHAR)(len - 1);
        buf[3] = (UCHAR)descLen;
    }

    if (len > allocLen) len = allocLen;
    AhciCopyToSrb(Srb, buf, len);
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

/* ------------------------------------------------------------------ */
/* ATA disk read/write                                                 */
/* ------------------------------------------------------------------ */

static VOID
AhciAtaReadWrite(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN AHCI_LBA Lba,
    IN ULONG Count,
    IN BOOLEAN IsWrite
)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    ATA_REQUEST req;
    ULONG result;
    UCHAR key, asc, ascq;

    /* A zero transfer length in READ/WRITE(10)/(16) moves nothing; ATA
       would read 0 as 256/65536 sectors, so never send it. */
    if (Count == 0) {
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return;
    }

    if (!pi->LbaSupported || !AhciLbaRangeValid(pi, Lba, Count)) {
        AHCI_DBG((AHCI_PFX "Port %lu: LBA %08lX:%08lX +%lu out of range\n", Port, Lba.High, Lba.Low, Count));
        AhciFailWithSense(HwInit, Port, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_LBA_OUT_OF_RANGE, 0);
        return;
    }

    if (Count > (Srb->DataTransferLength / pi->SectorSize)) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return;
    }

    ZeroMemoryBytes(&req, sizeof(req));
    req.DataBuffer = Srb->DataBuffer;
    req.DataBufferLen = Count * pi->SectorSize;
    req.IsWrite = IsWrite;
    req.Lba = Lba;
    req.SectorCount = Count;

    /* 28-bit when possible, EXT only on LBA48 devices when needed */
    if (AhciLbaFits28(Lba, Count)) {
        req.Is48Bit = FALSE;
        req.Command = IsWrite ? IDE_COMMAND_WRITE_DMA : IDE_COMMAND_READ_DMA;
    } else if (pi->Lba48 && Count <= ATA_LBA48_MAX_SECTORS) {
        req.Is48Bit = TRUE;
        req.Command = IsWrite ? IDE_COMMAND_WRITE_DMA_EXT : IDE_COMMAND_READ_DMA_EXT;
    } else {
        /* Cannot happen for a valid range on a 28-bit-only device (its
           capacity is < 2^28) unless the count is out of bounds. */
        AhciFailWithSense(HwInit, Port, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_INVALID_CDB_FIELD, 0);
        return;
    }

    result = AhciRunCommand(HwInit, Port, Srb, &req);

    switch (result) {
    case AHCI_CMD_OK:
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        break;
    case AHCI_CMD_BAD_BUFFER:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    default:
        AhciAtaErrorToSense(pi->LastTfd, &key, &asc, &ascq);
        AhciFailWithSense(HwInit, Port, Srb, key, asc, ascq);
        break;
    }
}

static VOID
AhciProcessAtaSrb(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PUCHAR cdb = Srb->Cdb;
    AHCI_LBA lba;
    ULONG count;

    switch (cdb[0]) {
    case SCSIOP_INQUIRY:
        AhciHandleInquiry(HwInit, Port, Srb);
        return;

    case SCSIOP_REQUEST_SENSE:
        AhciHandleRequestSenseAta(HwInit, Port, Srb);
        return;

    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_MEDIUM_REMOVAL:
    case SCSIOP_SYNCHRONIZE_CACHE:
    case AHCI_SCSIOP_SYNC_CACHE16:
    case SCSIOP_VERIFY:
    case AHCI_SCSIOP_VERIFY16:
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return;

    case SCSIOP_READ_CAPACITY:
        AhciHandleReadCapacity10(HwInit, Port, Srb);
        return;

    case AHCI_SCSIOP_SERVICE_ACTION_IN16:
        if ((cdb[1] & 0x1F) == AHCI_SA_READ_CAPACITY16) {
            AhciHandleReadCapacity16(HwInit, Port, Srb);
            return;
        }
        break;

    case SCSIOP_MODE_SENSE:
    case SCSIOP_MODE_SENSE10:
        AhciHandleModeSenseAta(HwInit, Port, Srb);
        return;

    case SCSIOP_READ6:
    case SCSIOP_WRITE6:
        lba.High = 0;
        lba.Low = ((ULONG)(cdb[1] & 0x1F) << 16) | ((ULONG)cdb[2] << 8) | cdb[3];
        count = cdb[4] ? cdb[4] : 256;
        AhciAtaReadWrite(HwInit, Port, Srb, lba, count, (BOOLEAN)(cdb[0] == SCSIOP_WRITE6));
        return;

    case SCSIOP_READ:
    case SCSIOP_WRITE:
        lba = AhciLbaFromBigEndian(&cdb[2], 4);
        count = ((ULONG)cdb[7] << 8) | cdb[8];
        AhciAtaReadWrite(HwInit, Port, Srb, lba, count, (BOOLEAN)(cdb[0] == SCSIOP_WRITE));
        return;

    case AHCI_SCSIOP_READ16:
    case AHCI_SCSIOP_WRITE16:
        lba = AhciLbaFromBigEndian(&cdb[2], 8);
        count = AhciBigEndian32(&cdb[10]);
        AhciAtaReadWrite(HwInit, Port, Srb, lba, count, (BOOLEAN)(cdb[0] == AHCI_SCSIOP_WRITE16));
        return;
    }

    Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
}

/* ------------------------------------------------------------------ */
/* ATAPI                                                               */
/* ------------------------------------------------------------------ */

/* REQUEST SENSE sent to the drive; returns the sense length obtained
   (0 on failure). Data is left in the port's bounce buffer. */
static ULONG
AhciAtapiFetchSense(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG AllocLen)
{
    UCHAR cdb[12];

    if (AllocLen == 0 || AllocLen > 252) AllocLen = sizeof(SENSE_DATA);
    AllocLen = (AllocLen + 1) & ~1UL;           /* PRD byte counts are even */

    ZeroMemoryBytes(cdb, sizeof(cdb));
    cdb[0] = SCSIOP_REQUEST_SENSE;
    cdb[4] = (UCHAR)AllocLen;

    if (AhciAtapiBounceCommand(HwInit, Port, cdb, AllocLen) != AHCI_CMD_OK) return 0;
    return AllocLen;
}

static VOID
AhciAtapiFailed(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    Srb->SrbStatus = SRB_STATUS_ERROR;
    Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;

#if AHCI_AUTOSENSE
    if (AhciAutoSenseWanted(Srb)) {
        ULONG tfd = HwInit->Ports[Port].LastTfd;
        ULONG n;

        /* Real auto-sense: ask the drive, like a SCSI HBA would */
        n = AhciAtapiFetchSense(HwInit, Port, sizeof(SENSE_DATA));
        if (n != 0) {
            AhciSetAutoSense(Srb, HwInit->Ports[Port].BounceBuffer, n);
        } else if (tfd & TFD_STS_ERR) {
            /* Drive would not answer: at least pass on its sense key */
            SENSE_DATA sense;
            AhciBuildSense(&sense, (UCHAR)ATAPI_ERROR_SENSE_KEY(tfd >> TFD_ERR_SHIFT), 0, 0);
            AhciSetAutoSense(Srb, &sense, sizeof(sense));
        }
    }
#else
    (VOID)HwInit;
    (VOID)Port;
#endif
}

static VOID
AhciAtapiRequestSense(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    ULONG allocLen = Srb->Cdb[4];
    ULONG n;
    SENSE_DATA empty;

    /* Sense the driver itself produced (e.g. unsupported VPD page) */
    if (pi->SensePending) {
        AhciBuildSense(&empty, pi->SenseKey, pi->SenseAsc, pi->SenseAscq);
        pi->SensePending = FALSE;
        AhciCopyToSrb(Srb, &empty, sizeof(empty));
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return;
    }

    if (allocLen > Srb->DataTransferLength) allocLen = Srb->DataTransferLength;

    n = AhciAtapiFetchSense(HwInit, Port, allocLen);
    if (n != 0) {
        AhciCopyToSrb(Srb, HwInit->Ports[Port].BounceBuffer, n);
    } else {
        /* The drive gave us nothing: an empty NO SENSE is the honest answer */
        AhciBuildSense(&empty, SCSI_SENSE_NO_SENSE, 0, 0);
        AhciCopyToSrb(Srb, &empty, sizeof(empty));
    }
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

/* ATAPI has no MODE SENSE(6): send (10), convert the header like atapi.sys */
static VOID
AhciAtapiModeSense6(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PUCHAR bounce = HwInit->Ports[Port].BounceBuffer;
    UCHAR cdb[12];
    UCHAR hdr6[4];
    ULONG xferLen, i;

    ZeroMemoryBytes(cdb, sizeof(cdb));
    cdb[0] = SCSIOP_MODE_SENSE10;
    cdb[2] = (UCHAR)(Srb->Cdb[2] & 0x3F);       /* page code */
    cdb[7] = 0;                                 /* ParameterListLengthMsb */
    cdb[8] = Srb->Cdb[4];                       /* SCSI-6 allocation length */

    /* Same data length as the SRB, through the bounce buffer */
    xferLen = Srb->DataTransferLength;
    if (xferLen > AHCI_BOUNCE_SIZE) xferLen = AHCI_BOUNCE_SIZE;
    xferLen = (xferLen + 1) & ~1UL;             /* PRD byte counts are even */

    if (AhciAtapiBounceCommand(HwInit, Port, cdb, xferLen) != AHCI_CMD_OK) {
        AhciAtapiFailed(HwInit, Port, Srb);
        return;
    }

    if (Srb->DataTransferLength >= sizeof(MODE_PARAMETER_HEADER_10)) {
        ULONG payload = Srb->DataTransferLength - sizeof(MODE_PARAMETER_HEADER_10);
        if (payload > AHCI_BOUNCE_SIZE - sizeof(MODE_PARAMETER_HEADER_10)) {
            payload = AHCI_BOUNCE_SIZE - sizeof(MODE_PARAMETER_HEADER_10);
        }

        hdr6[0] = bounce[1];                    /* ModeDataLengthLsb */
        hdr6[1] = bounce[2];                    /* MediumType */
        hdr6[2] = 0;                            /* DeviceSpecificParameter */
        hdr6[3] = 0;                            /* BlockDescriptorLength */
        for (i = 0; i < payload; i++) bounce[4 + i] = bounce[8 + i];
        for (i = 0; i < 4; i++) bounce[i] = hdr6[i];
    }

    AhciCopyToSrb(Srb, bounce, Srb->DataTransferLength);
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

static VOID
AhciProcessAtapiSrb(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    ATA_REQUEST req;
    ULONG result;

    switch (Srb->Cdb[0]) {
    case SCSIOP_INQUIRY:
        AhciHandleInquiry(HwInit, Port, Srb);
        return;

    case SCSIOP_REQUEST_SENSE:
        AhciAtapiRequestSense(HwInit, Port, Srb);
        return;

    case SCSIOP_SYNCHRONIZE_CACHE:
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return;

    case SCSIOP_MODE_SENSE:
        AhciAtapiModeSense6(HwInit, Port, Srb);
        return;
    }

    /* Everything else (TEST UNIT READY, READ(10/12), READ TOC, MODE
       SENSE(10), START STOP UNIT, ...) is a native ATAPI packet. */
    ZeroMemoryBytes(&req, sizeof(req));
    req.IsAtapi = TRUE;
    req.DataBuffer = Srb->DataBuffer;
    req.DataBufferLen = (Srb->DataBuffer != NULL) ? Srb->DataTransferLength : 0;
    req.IsWrite = (BOOLEAN)((Srb->SrbFlags & SRB_FLAGS_DATA_OUT) != 0);
    req.Cdb = Srb->Cdb;
    req.CdbLength = Srb->CdbLength;

    result = AhciRunCommand(HwInit, Port, Srb, &req);
    if (result == AHCI_CMD_OK) {
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
    } else if (result == AHCI_CMD_BAD_BUFFER) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
    } else {
        AhciAtapiFailed(HwInit, Port, Srb);
    }
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

VOID
AhciSatlProcessSrb(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb)
{
    ULONG port = Srb->TargetId;

    if (Srb->PathId != 0 || port >= MAX_SUPPORTED_PORTS || !HwInit->Ports[port].Present) {
        Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        return;
    }
    if (Srb->Lun != 0) {
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        return;
    }

    /* Sense belongs to the command that failed: any other command
       discards it, so a later REQUEST SENSE never reports a stale error. */
    if (Srb->Cdb[0] != SCSIOP_REQUEST_SENSE) {
        HwInit->Ports[port].SensePending = FALSE;
    }

    if (HwInit->Ports[port].IsAtapi) {
        AhciProcessAtapiSrb(HwInit, port, Srb);
    } else {
        AhciProcessAtaSrb(HwInit, port, Srb);
    }
}
