#include "ahcint.h"

/* ahcisatl.c - SCSI-to-ATA translation, ATAPI pass-through, miniport IOCTLs */

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

static VOID
AhciLbaAdd(IN OUT PAHCI_LBA Lba, IN ULONG Count)
{
    ULONG low = Lba->Low + Count;
    if (low < Lba->Low) Lba->High++;
    Lba->Low = low;
}

/* Last addressable LBA (capacity - 1); 0 for an empty/unknown device. */
static AHCI_LBA
AhciLastLba(IN PAHCI_DEVICE Dev)
{
    AHCI_LBA last = Dev->TotalSectors;

    if (last.Low == 0 && last.High == 0) return last;
    if (last.Low == 0) last.High--;
    last.Low--;
    return last;
}

/* Lba + Count <= capacity ? */
static BOOLEAN
AhciLbaRangeValid(IN PAHCI_DEVICE Dev, IN AHCI_LBA Lba, IN ULONG Count)
{
    ULONG endLow, endHigh;

    endLow = Lba.Low + Count;
    endHigh = Lba.High;
    if (endLow < Lba.Low) {
        if (endHigh == 0xFFFFFFFF) return FALSE;
        endHigh++;
    }

    if (endHigh != Dev->TotalSectors.High) return (BOOLEAN)(endHigh < Dev->TotalSectors.High);
    return (BOOLEAN)(endLow <= Dev->TotalSectors.Low);
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

/* Called after every IDENTIFY: addressing, capacity, sector size and
   the optional features the SATL uses. */
VOID
AhciParseIdentify(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun)
{
    PAHCI_PORT_INFO pi;
    PAHCI_DEVICE dev;
    PUSHORT id;
    AHCI_LBA lba48;
    BOOLEAN w83valid;

    pi = &HwInit->Ports[Port];
    dev = AHCI_DEVICE_OF(HwInit, Port, Lun);
    id = (PUSHORT)pi->BounceBuffer;

    dev->LbaSupported = FALSE;
    dev->Lba48 = FALSE;
    dev->Ncq = FALSE;
    dev->Fua = FALSE;
    dev->FlushExt = FALSE;
    dev->NcqDepth = 1;
    dev->TotalSectors.Low = 0;
    dev->TotalSectors.High = 0;
    dev->SectorSize = AHCI_SECTOR_SIZE;
    dev->PhysExponent = 0;
    dev->Trim = FALSE;
    dev->TrimZeroes = FALSE;
    dev->Smart = FALSE;
    dev->RotationRate = 0;
    dev->FormFactor = 0;

    if (dev->IdentifyValid) {
        ULONG i;
        dev->Id[AHCI_ID_CONFIG] = id[IDW_GENERAL_CONFIG];
        dev->Id[AHCI_ID_CYLINDERS] = id[IDW_CYLINDERS];
        dev->Id[AHCI_ID_HEADS] = id[IDW_HEADS];
        dev->Id[AHCI_ID_SPT] = id[IDW_SECTORS_PER_TRACK];
        dev->Id[AHCI_ID_CMDSET_1] = id[IDW_CMDSET_SUPPORTED_1];
        dev->Id[AHCI_ID_CMDSET_EXT] = id[IDW_CMDSET_SUPPORTED_EXT];
        dev->Id[AHCI_ID_ENABLED_1] = id[IDW_CMDSET_ENABLED_1];
        for (i = 0; i < 10; i++) dev->Id[AHCI_ID_SERIAL + i] = id[IDW_SERIAL + i];
        for (i = 0; i < 4; i++) dev->Id[AHCI_ID_FIRMWARE + i] = id[IDW_FIRMWARE + i];
        for (i = 0; i < 20; i++) dev->Id[AHCI_ID_MODEL + i] = id[IDW_MODEL + i];
    }

    if (!dev->IdentifyValid || dev->IsAtapi) return;

    /* Word 106: logical sector size (words 117-118) and logical sectors per
       physical sector; all LBAs and counts are in logical sectors. */
    if ((id[IDW_SECTOR_SIZE_INFO] & IDW106_VALID_MASK) == IDW106_VALID) {
        if (id[IDW_SECTOR_SIZE_INFO] & IDW106_LONG_LOGICAL) {
            ULONG bytes = ((ULONG)id[IDW_LOGICAL_SECTOR_WORDS] |
                           ((ULONG)id[IDW_LOGICAL_SECTOR_WORDS + 1] << 16)) * 2;
            if (bytes >= AHCI_SECTOR_SIZE && bytes <= AHCI_MAX_SECTOR_SIZE &&
                (bytes & (bytes - 1)) == 0) {
                dev->SectorSize = bytes;
            } else {
                AHCI_DBG((AHCI_PFX "Port %lu.%lu: logical sector of %lu bytes not supported\n",
                          Port, (ULONG)dev->Pmp, bytes));
                dev->IdentifyValid = FALSE;
                return;
            }
        }
        if (id[IDW_SECTOR_SIZE_INFO] & IDW106_MULTI_LOGICAL) {
            dev->PhysExponent = (UCHAR)(id[IDW_SECTOR_SIZE_INFO] & IDW106_PHYS_EXP_MASK);
        }
    }

    dev->LbaSupported = (BOOLEAN)((id[IDW_CAPABILITIES] & IDW49_LBA_SUPPORTED) != 0);
    w83valid = (BOOLEAN)((id[IDW_CMDSET_SUPPORTED_2] & IDW83_VALID_MASK) == IDW83_VALID);

    /* LBA48: word 83 valid (15:14 = 01) with bit 10, and words 100-103 != 0 */
    lba48.Low = (ULONG)id[IDW_LBA48_SECTORS] | ((ULONG)id[IDW_LBA48_SECTORS + 1] << 16);
    lba48.High = (ULONG)id[IDW_LBA48_SECTORS + 2] | ((ULONG)id[IDW_LBA48_SECTORS + 3] << 16);

    if (dev->LbaSupported && w83valid && (id[IDW_CMDSET_SUPPORTED_2] & IDW83_LBA48) &&
        (lba48.Low != 0 || lba48.High != 0)) {
        dev->Lba48 = TRUE;
        dev->TotalSectors = lba48;
    } else if (dev->LbaSupported) {
        dev->TotalSectors.Low = (ULONG)id[IDW_LBA28_SECTORS] | ((ULONG)id[IDW_LBA28_SECTORS + 1] << 16);
    }

    dev->FlushExt = (BOOLEAN)(dev->Lba48 && w83valid && (id[IDW_CMDSET_SUPPORTED_2] & IDW83_FLUSH_EXT));
    dev->Fua = (BOOLEAN)(dev->Lba48 &&
                         (id[IDW_CMDSET_SUPPORTED_EXT] & IDW83_VALID_MASK) == IDW83_VALID &&
                         (id[IDW_CMDSET_SUPPORTED_EXT] & IDW84_FUA));

    /* TRIM (a 48-bit DMA command) and what the medium is */
    dev->Trim = (BOOLEAN)(dev->Lba48 && (id[IDW_DSM_SUPPORT] & IDW169_TRIM) && id[IDW_DSM_SUPPORT] != 0xFFFF);
    dev->TrimZeroes = (BOOLEAN)(dev->Trim && (id[IDW_ADDITIONAL_SUPPORTED] & IDW69_DETERMINISTIC_TRIM) &&
                                (id[IDW_ADDITIONAL_SUPPORTED] & IDW69_ZERO_AFTER_TRIM));
    if (id[IDW_ROTATION_RATE] == 1 || (id[IDW_ROTATION_RATE] >= 0x0401 && id[IDW_ROTATION_RATE] <= 0xFFFE)) {
        dev->RotationRate = id[IDW_ROTATION_RATE];
    }
    dev->FormFactor = (UCHAR)(id[IDW_FORM_FACTOR] & 0x0F);
    dev->Smart = (BOOLEAN)((id[IDW_CMDSET_SUPPORTED_1] & IDW82_SMART) && (id[IDW_CMDSET_ENABLED_1] & IDW82_SMART) &&
                           id[IDW_CMDSET_SUPPORTED_1] != 0xFFFF);

    /* NCQ: device and HBA support it, the disk sits directly on the port
       (command-based switching serializes a port multiplier anyway) */
    if (dev->Lba48 && (id[IDW_SATA_CAPABILITIES] & IDW76_NCQ) &&
        id[IDW_SATA_CAPABILITIES] != 0xFFFF && !pi->PmAttached &&
        HwInit->AsyncMode && dev->NcqErrors < AHCI_NCQ_ERROR_LIMIT) {
        dev->NcqDepth = (UCHAR)((id[IDW_QUEUE_DEPTH] & 0x1F) + 1);
        dev->Ncq = (BOOLEAN)(dev->NcqDepth > 1);
    }

    AHCI_DBG((AHCI_PFX "Port %lu.%lu: LBA=%s LBA48=%s (W83=%04lX W86=%04lX) sectors=%08lX:%08lX x %lu bytes, %lu per physical\n",
              Port, (ULONG)dev->Pmp,
              dev->LbaSupported ? "yes" : "no",
              dev->Lba48 ? "yes" : "no",
              (ULONG)id[IDW_CMDSET_SUPPORTED_2], (ULONG)id[IDW_CMDSET_ENABLED_2],
              dev->TotalSectors.High, dev->TotalSectors.Low,
              dev->SectorSize, 1UL << dev->PhysExponent));
    AHCI_DBG((AHCI_PFX "Port %lu.%lu: NCQ=%lu (depth %lu) FUA=%s write cache=%s\n",
              Port, (ULONG)dev->Pmp, (ULONG)dev->Ncq, (ULONG)dev->NcqDepth,
              dev->Fua ? "yes" : "no",
              (id[IDW_CMDSET_ENABLED_1] & IDW82_WRITE_CACHE) ? "on" : "off"));
    AHCI_DBG((AHCI_PFX "Port %lu.%lu: TRIM=%s%s rotation %04lX SMART=%s\n", Port, (ULONG)dev->Pmp,
              dev->Trim ? "yes" : "no", dev->TrimZeroes ? " (zeroes)" : "", (ULONG)dev->RotationRate,
              dev->Smart ? "yes" : "no"));

    if (!dev->LbaSupported) {
        /* SATA mandates LBA; a CHS-only device is not addressable here. */
        AHCI_DBG((AHCI_PFX "Port %lu.%lu: device has no LBA support, not usable\n", Port, (ULONG)dev->Pmp));
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

static VOID
AhciCopyToSrb(IN PSCSI_REQUEST_BLOCK Srb, IN PVOID Data, IN ULONG Length)
{
    ULONG n = Length;

    if (Srb->DataBuffer == NULL) return;
    if (n > Srb->DataTransferLength) n = Srb->DataTransferLength;
    ZeroMemoryBytes(Srb->DataBuffer, Srb->DataTransferLength);
    CopyMemoryBytes(Srb->DataBuffer, Data, n);
}

static VOID
AhciGood(IN PSCSI_REQUEST_BLOCK Srb)
{
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

/* CHECK CONDITION with the given sense: auto-sense, or kept for the next
   REQUEST SENSE */
static VOID
AhciFailWithRawSense(IN PAHCI_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb, IN PVOID Sense, IN ULONG Length)
{
    Srb->SrbStatus = SRB_STATUS_ERROR;
    Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;

#if AHCI_AUTOSENSE
    if (Srb->SenseInfoBuffer != NULL && Srb->SenseInfoBufferLength != 0 &&
        !(Srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE)) {
        ULONG n = Length;
        if (n > Srb->SenseInfoBufferLength) n = Srb->SenseInfoBufferLength;
        ZeroMemoryBytes(Srb->SenseInfoBuffer, Srb->SenseInfoBufferLength);
        CopyMemoryBytes(Srb->SenseInfoBuffer, Sense, n);
        Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
        return;
    }
#endif

    if (Length > AHCI_SENSE_MAX) Length = AHCI_SENSE_MAX;
    CopyMemoryBytes(Dev->Sense, Sense, Length);
    Dev->SenseLength = (UCHAR)Length;
    Dev->SensePending = TRUE;
}

static VOID
AhciFailWithSense(IN PAHCI_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb, IN UCHAR Key, IN UCHAR Asc, IN UCHAR Ascq)
{
    SENSE_DATA sense;

    if (Key == SCSI_SENSE_NO_SENSE) {           /* nothing specific to report */
        Srb->SrbStatus = SRB_STATUS_ERROR;
        Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        return;
    }
    AhciBuildSense(&sense, Key, Asc, Ascq);
    AhciFailWithRawSense(Dev, Srb, &sense, sizeof(sense));
}

static VOID
AhciInvalidField(IN PAHCI_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    AhciFailWithSense(Dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_INVALID_CDB_FIELD, 0);
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

/* A failed ATA disk command: sense from the device's last task file */
VOID
AhciSatlCompleteAtaError(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Srb->Lun);
    UCHAR key, asc, ascq;

    AhciAtaErrorToSense(dev->LastTfd, &key, &asc, &ascq);
    AhciFailWithSense(dev, Srb, key, asc, ascq);
}

static VOID
AhciCompleteResult(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb, IN ULONG Result)
{
    switch (Result) {
    case AHCI_CMD_OK:
        AhciGood(Srb);
        break;
    case AHCI_CMD_BAD_BUFFER:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    default:
        AhciSatlCompleteAtaError(HwInit, Port, Srb);
        break;
    }
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

/* EVPD: supported-pages list and unit serial number (IDENTIFY 10..19) */
/* Peripheral device type of an ATAPI device: IDENTIFY PACKET DEVICE word 0
   bits 12:8 (05h CD/DVD, 00h direct access such as ZIP, LS-120 or
   SuperDisk, 07h optical memory, 01h tape); 1Fh (unknown) and a device
   without IDENTIFY data count as CD/DVD */
static UCHAR
AhciAtapiDeviceType(IN PAHCI_DEVICE Dev)
{
    UCHAR type;

    if (!Dev->IdentifyValid) return READ_ONLY_DIRECT_ACCESS_DEVICE;
    type = (UCHAR)((Dev->Id[AHCI_ID_CONFIG] >> 8) & 0x1F);
    return (type == 0x1F) ? (UCHAR)READ_ONLY_DIRECT_ACCESS_DEVICE : type;
}

static VOID
AhciHandleInquiryVpd(IN PHW_DEVICE_EXTENSION HwInit, IN PAHCI_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR page[64];
    ULONG len, allocLen = ((ULONG)Srb->Cdb[3] << 8) | Srb->Cdb[4];
    BOOLEAN unmap = (BOOLEAN)(Dev->Trim && HwInit->TrimAllowed);

    ZeroMemoryBytes(page, sizeof(page));
    page[0] = Dev->IsAtapi ? AhciAtapiDeviceType(Dev) : DIRECT_ACCESS_DEVICE;
    page[1] = Srb->Cdb[2];

    if (Dev->IsAtapi && Srb->Cdb[2] != AHCI_VPD_SUPPORTED_PAGES && Srb->Cdb[2] != AHCI_VPD_SERIAL_NUMBER) {
        AhciInvalidField(Dev, Srb);
        return;
    }
    switch (Srb->Cdb[2]) {
    case AHCI_VPD_SUPPORTED_PAGES:
        page[4] = AHCI_VPD_SUPPORTED_PAGES;
        page[5] = AHCI_VPD_SERIAL_NUMBER;
        len = 6;
        if (!Dev->IsAtapi) {
            page[len++] = AHCI_VPD_BLOCK_LIMITS;
            page[len++] = AHCI_VPD_BLOCK_CHARACTERISTICS;
            page[len++] = AHCI_VPD_PROVISIONING;
        }
        page[3] = (UCHAR)(len - 4);
        break;

    case AHCI_VPD_BLOCK_LIMITS:
        page[3] = 0x3C;
        AhciPutBigEndian32(&page[8], AHCI_MAX_TRANSFER / Dev->SectorSize);
        AhciPutBigEndian32(&page[12], AHCI_MAX_TRANSFER / Dev->SectorSize);
        if (unmap) {
            AhciPutBigEndian32(&page[20], AHCI_UNMAP_MAX_BLOCKS);
            AhciPutBigEndian32(&page[24], AHCI_UNMAP_MAX_DESCRIPTORS);
            AhciPutBigEndian32(&page[28], 1UL << Dev->PhysExponent);
        }
        len = 4 + 0x3C;
        break;

    case AHCI_VPD_BLOCK_CHARACTERISTICS:
        page[3] = 0x3C;
        page[4] = (UCHAR)(Dev->RotationRate >> 8);
        page[5] = (UCHAR)Dev->RotationRate;
        page[7] = Dev->FormFactor;
        len = 4 + 0x3C;
        break;

    case AHCI_VPD_PROVISIONING:
        page[3] = 4;
        page[5] = (UCHAR)((unmap ? 0x80 : 0) | ((unmap && Dev->TrimZeroes) ? 0x04 : 0));
        len = 4 + 4;
        break;

    case AHCI_VPD_SERIAL_NUMBER:
        page[3] = 20;
        if (Dev->IdentifyValid) {
            AhciExtractAtaString(Dev->Id, AHCI_ID_SERIAL, 10, &page[4], 20);
        } else {
            ULONG i;
            for (i = 0; i < 20; i++) page[4 + i] = ' ';
        }
        len = 4 + 20;
        break;

    default:
        AhciInvalidField(Dev, Srb);
        return;
    }

    if (len > allocLen) len = allocLen;
    AhciCopyToSrb(Srb, page, len);
    AhciGood(Srb);
}

static VOID
AhciHandleInquiry(IN PHW_DEVICE_EXTENSION HwInit, IN PAHCI_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    PUSHORT id = Dev->Id;
    INQUIRYDATA inq;
    PUCHAR v;
    PUCHAR p;
    ULONG i;

    if (Srb->Cdb[1] & 0x01) {
        AhciHandleInquiryVpd(HwInit, Dev, Srb);
        return;
    }
    if (Srb->Cdb[2] != 0) {                     /* page code without EVPD */
        AhciInvalidField(Dev, Srb);
        return;
    }

    ZeroMemoryBytes(&inq, sizeof(inq));

    inq.DeviceType = Dev->IsAtapi ? AhciAtapiDeviceType(Dev) : DIRECT_ACCESS_DEVICE;
    /* ATAPI: word 0 bit 7, and every CD/DVD drive as before */
    inq.RemovableMedia = (Dev->IsAtapi && inq.DeviceType == READ_ONLY_DIRECT_ACCESS_DEVICE) ? 1 :
                         ((id[AHCI_ID_CONFIG] & (1 << 7)) ? 1 : 0);
    /* ATAPI stays unversioned */
    inq.Versions = Dev->IsAtapi ? 0 : AHCI_INQUIRY_VERSION_SPC3;
    inq.ResponseDataFormat = 2;
    inq.AdditionalLength = 31;
    /* Tagged requests only where they become NCQ commands */
    inq.CommandQueue = (UCHAR)((HwInit->AsyncMode && Dev->Ncq) ? 1 : 0);

    if (Dev->IdentifyValid) {
        AhciExtractAtaString(id, AHCI_ID_MODEL, 4, inq.VendorId, 8);
        AhciExtractAtaString(id, AHCI_ID_MODEL + 4, 8, inq.ProductId, 16);
        AhciExtractAtaString(id, AHCI_ID_FIRMWARE, 2, inq.ProductRevisionLevel, 4);
    } else {
        v = (PUCHAR)(Dev->IsAtapi ? "ATAPI   " : "ATA     ");
        p = (PUCHAR)(Dev->IsAtapi ? "SATA CD-ROM     " : "SATA HARDDISK   ");
        for (i = 0; i < 8; i++) inq.VendorId[i] = v[i];
        for (i = 0; i < 16; i++) inq.ProductId[i] = p[i];
        inq.ProductRevisionLevel[0] = '1';
        inq.ProductRevisionLevel[1] = '.';
        inq.ProductRevisionLevel[2] = '0';
        inq.ProductRevisionLevel[3] = '0';
    }

    AhciCopyToSrb(Srb, &inq, sizeof(inq));
    AhciGood(Srb);
}

/* REPORT LUNS: LUN 0 for a direct device, one per device behind a PM */
static VOID
AhciHandleReportLuns(IN PAHCI_PORT_INFO Pi, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR data[8 + 8 * AHCI_MAX_LUNS];
    ULONG n = Pi->DeviceCount;
    ULONG allocLen = AhciBigEndian32(&Srb->Cdb[6]);
    ULONG i, len;

    ZeroMemoryBytes(data, sizeof(data));
    AhciPutBigEndian32(&data[0], n * 8);
    for (i = 0; i < n; i++) data[8 + i * 8 + 1] = (UCHAR)i;
    len = 8 + n * 8;
    if (len > allocLen) len = allocLen;
    AhciCopyToSrb(Srb, data, len);
    AhciGood(Srb);
}

/* READ CAPACITY(10) for 2 TB and more (last LBA FFFFFFFFh or beyond):
   FFFFFFFFh sends a class driver that knows READ CAPACITY(16) there.
   One that does not (NT 3.x/4.0, 9x, 2000, XP x86) adds one to it in 32
   bits and sees a disk of 0 bytes; it gets FFFFFFFEh, the most it can
   address with READ(10), and sees the first 2 TB. */
static VOID
AhciHandleReadCapacity10(IN PHW_DEVICE_EXTENSION HwInit, IN PAHCI_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR data[8];
    AHCI_LBA last = AhciLastLba(Dev);

    if (last.High != 0 || last.Low == 0xFFFFFFFF) {
        last.Low = HwInit->Capacity16 ? 0xFFFFFFFF : 0xFFFFFFFE;
    }
    AhciPutBigEndian32(&data[0], last.Low);
    AhciPutBigEndian32(&data[4], Dev->SectorSize);

    AhciCopyToSrb(Srb, data, sizeof(data));
    AhciGood(Srb);
}

/* SERVICE ACTION IN(16) / READ CAPACITY(16): full 64-bit last LBA */
static VOID
AhciHandleReadCapacity16(IN PHW_DEVICE_EXTENSION HwInit, IN PAHCI_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR data[AHCI_READ_CAPACITY16_DATA_LEN];
    AHCI_LBA last = AhciLastLba(Dev);
    ULONG allocLen = AhciBigEndian32(&Srb->Cdb[10]);
    ULONG n;

    ZeroMemoryBytes(data, sizeof(data));
    AhciPutBigEndian32(&data[0], last.High);
    AhciPutBigEndian32(&data[4], last.Low);
    AhciPutBigEndian32(&data[8], Dev->SectorSize);
    data[13] = (UCHAR)(Dev->PhysExponent & 0x0F);
    if (Dev->Trim && HwInit->TrimAllowed) {
        data[14] = (UCHAR)(0x80 | (Dev->TrimZeroes ? 0x40 : 0));        /* LBPME, LBPRZ */
    }

    n = sizeof(data);
    if (n > allocLen) n = allocLen;
    AhciCopyToSrb(Srb, data, n);
    AhciGood(Srb);
}

static VOID
AhciHandleRequestSenseAta(IN PAHCI_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    SENSE_DATA sense;

    if (Dev->SensePending) {
        AhciCopyToSrb(Srb, Dev->Sense, Dev->SenseLength);
        Dev->SensePending = FALSE;
    } else {
        AhciBuildSense(&sense, SCSI_SENSE_NO_SENSE, 0, 0);
        AhciCopyToSrb(Srb, &sense, sizeof(sense));
    }
    AhciGood(Srb);
}

/* MODE SENSE for ATA disks: block descriptor, rigid geometry (04h),
   caching (08h) and control (0Ah) pages. DPOFUA when FUA writes exist. */
static VOID
AhciHandleModeSenseAta(IN PHW_DEVICE_EXTENSION HwInit, IN PAHCI_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    PUSHORT id = Dev->Id;
    BOOLEAN ten = (BOOLEAN)(Srb->Cdb[0] == SCSIOP_MODE_SENSE10);
    BOOLEAN dbd = (BOOLEAN)((Srb->Cdb[1] & 0x08) != 0);
    BOOLEAN llba = (BOOLEAN)(ten && (Srb->Cdb[1] & 0x10) != 0);    /* LLBAA */
    UCHAR pageCode = (UCHAR)(Srb->Cdb[2] & 0x3F);
    UCHAR pc = (UCHAR)(Srb->Cdb[2] >> 6);           /* 1 = changeable values */
    ULONG allocLen;
    UCHAR buf[96];
    ULONG hdrLen, len, descLen;
    ULONG blocks, cyls, heads, spt;
    PUCHAR p;
    BOOLEAN any = FALSE;

    allocLen = ten ? (((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8]) : Srb->Cdb[4];

    ZeroMemoryBytes(buf, sizeof(buf));
    hdrLen = ten ? 8 : 4;
    len = hdrLen;

    descLen = 0;
    if (!dbd && llba) {
        /* Long LBA block descriptor (MODE SENSE(10) only): 64-bit count */
        p = &buf[len];
        AhciPutBigEndian32(&p[0], Dev->TotalSectors.High);
        AhciPutBigEndian32(&p[4], Dev->TotalSectors.Low);
        AhciPutBigEndian32(&p[12], Dev->SectorSize);
        descLen = 16;
        len += 16;
    } else if (!dbd) {
        /* Short block descriptor: 24-bit block count, saturating */
        blocks = (Dev->TotalSectors.High != 0 || Dev->TotalSectors.Low > 0xFFFFFF)
                     ? 0xFFFFFF : Dev->TotalSectors.Low;
        p = &buf[len];
        p[1] = (UCHAR)(blocks >> 16);
        p[2] = (UCHAR)(blocks >> 8);
        p[3] = (UCHAR)blocks;
        p[5] = (UCHAR)(Dev->SectorSize >> 16);
        p[6] = (UCHAR)(Dev->SectorSize >> 8);
        p[7] = (UCHAR)Dev->SectorSize;
        descLen = 8;
        len += 8;
    }

    if (pageCode == AHCI_MODE_PAGE_GEOMETRY || pageCode == AHCI_MODE_PAGE_ALL) {
        cyls = heads = spt = 0;
        if (Dev->IdentifyValid) {
            cyls = id[AHCI_ID_CYLINDERS];
            heads = id[AHCI_ID_HEADS];
            spt = id[AHCI_ID_SPT];
        }
        if (cyls == 0 || heads == 0 || spt == 0) {
            ULONG total32 = (Dev->TotalSectors.High != 0) ? 0xFFFFFFFF : Dev->TotalSectors.Low;
            heads = 255;
            spt = 63;
            cyls = total32 / (heads * spt);
            if (cyls == 0) cyls = 1;
        }
        p = &buf[len];
        p[0] = AHCI_MODE_PAGE_GEOMETRY;
        p[1] = 0x16;
        if (pc != 1) {
            p[2] = (UCHAR)(cyls >> 16);
            p[3] = (UCHAR)(cyls >> 8);
            p[4] = (UCHAR)cyls;
            p[5] = (UCHAR)heads;
            p[20] = (UCHAR)(spt >> 8);      /* rotation-rate field */
            p[21] = (UCHAR)spt;
        }
        len += 24;
        any = TRUE;
    }

    if (pageCode == AHCI_MODE_PAGE_CACHING || pageCode == AHCI_MODE_PAGE_ALL) {
        p = &buf[len];
        p[0] = AHCI_MODE_PAGE_CACHING;
        p[1] = 0x12;
        if (pc == 1) {
            if (id[AHCI_ID_CMDSET_1] & IDW82_WRITE_CACHE) p[2] |= 0x04;
            if (id[AHCI_ID_CMDSET_1] & IDW82_LOOKAHEAD) p[12] |= 0x20;
        } else {
            if (id[AHCI_ID_ENABLED_1] & IDW82_WRITE_CACHE) p[2] |= 0x04;            /* WCE */
            if (!(id[AHCI_ID_ENABLED_1] & IDW82_LOOKAHEAD)) p[12] |= 0x20;         /* DRA */
        }
        len += 20;
        any = TRUE;
    }

    if (pageCode == AHCI_MODE_PAGE_CONTROL || pageCode == AHCI_MODE_PAGE_ALL) {
        p = &buf[len];
        p[0] = AHCI_MODE_PAGE_CONTROL;
        p[1] = 0x0A;
        len += 12;
        any = TRUE;
    }

    if (!any && pageCode != 0) {
        AhciInvalidField(Dev, Srb);
        return;
    }

    if (ten) {
        buf[0] = (UCHAR)((len - 2) >> 8);
        buf[1] = (UCHAR)(len - 2);
        buf[3] = (UCHAR)((Dev->Fua || (HwInit->AsyncMode && Dev->Ncq)) ? 0x10 : 0x00);  /* DPOFUA */
        buf[4] = (UCHAR)((descLen == 16) ? 0x01 : 0x00);    /* LONGLBA */
        buf[7] = (UCHAR)descLen;
    } else {
        buf[0] = (UCHAR)(len - 1);
        buf[2] = (UCHAR)((Dev->Fua || (HwInit->AsyncMode && Dev->Ncq)) ? 0x10 : 0x00);
        buf[3] = (UCHAR)descLen;
    }

    if (len > allocLen) len = allocLen;
    AhciCopyToSrb(Srb, buf, len);
    AhciGood(Srb);
}

/* MODE SELECT: caching page only -- WCE and DRA become SET FEATURES.
   Other pages are accepted unchanged. */
static VOID
AhciHandleModeSelectAta(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Lun);
    BOOLEAN ten = (BOOLEAN)(Srb->Cdb[0] == AHCI_SCSIOP_MODE_SELECT10);
    PUCHAR data = (PUCHAR)Srb->DataBuffer;
    ULONG listLen, pos, hdrLen, pageLen;
    BOOLEAN wce, dra;

    listLen = ten ? (((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8]) : Srb->Cdb[4];
    if (listLen > Srb->DataTransferLength) listLen = Srb->DataTransferLength;
    if (listLen == 0 || data == NULL) {
        AhciGood(Srb);
        return;
    }

    hdrLen = ten ? 8 : 4;
    if (listLen < hdrLen) {
        AhciFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_PARAM_LIST_LENGTH, 0);
        return;
    }
    pos = hdrLen + (ten ? (((ULONG)data[6] << 8) | data[7]) : data[3]);

    while (pos + 2 <= listLen) {
        pageLen = (ULONG)data[pos + 1] + 2;
        if (pos + pageLen > listLen) break;
        if ((data[pos] & 0x3F) == AHCI_MODE_PAGE_CACHING && pageLen >= 13) {
            wce = (BOOLEAN)((data[pos + 2] & 0x04) != 0);
            dra = (BOOLEAN)((data[pos + 12] & 0x20) != 0);

            if (dev->Id[AHCI_ID_CMDSET_1] & IDW82_WRITE_CACHE) {
                pi->WantWriteCache[dev->Pmp & 0x0F] = (UCHAR)(wce ? 1 : 2);
            } else if (wce) {
                AhciFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_INVALID_PARAM_FIELD, 0);
                return;
            }
            if (dev->Id[AHCI_ID_CMDSET_1] & IDW82_LOOKAHEAD) {
                pi->WantLookAhead[dev->Pmp & 0x0F] = (UCHAR)(dra ? 2 : 1);
            }
            AhciApplyDeviceSettings(HwInit, Port, Lun);
            /* IDENTIFY again: word 85 shows what is in effect now */
            if (AhciIdentifyDevice(HwInit, Port, Lun)) AhciParseIdentify(HwInit, Port, Lun);
        }
        pos += pageLen;
    }
    AhciGood(Srb);
}

/* ------------------------------------------------------------------ */
/* ATA disk commands                                                   */
/* ------------------------------------------------------------------ */

BOOLEAN
AhciFlushDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun)
{
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Lun);
    PUSHORT id = dev->Id;

    if (!dev->Present || dev->IsAtapi || !dev->IdentifyValid) return TRUE;
    /* Nothing to flush without a write cache */
    if (!(id[AHCI_ID_CMDSET_1] & IDW82_WRITE_CACHE)) return TRUE;

    return (BOOLEAN)(AhciRunSimple(HwInit, Port, Lun,
                                   (UCHAR)(dev->FlushExt ? IDE_COMMAND_FLUSH_CACHE_EXT : IDE_COMMAND_FLUSH_CACHE),
                                   0, AHCI_POLL_TIMEOUT_POWER_SEC) == AHCI_CMD_OK);
}

/* System shutdown: write cache flushed. With STANDBY=1 also STANDBY
   IMMEDIATE, so the drive parks its heads (or an SSD saves its tables)
   before the power goes; a request after it spins the drive up again. */
VOID
AhciShutdownDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun)
{
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Lun);

    if (!dev->Present || dev->IsAtapi || !dev->IdentifyValid) return;
    AhciFlushDevice(HwInit, Port, Lun);
    if (!HwInit->StandbyAtShutdown) return;
    if (AhciRunSimple(HwInit, Port, Lun, IDE_COMMAND_STANDBY_IMMEDIATE, 0, AHCI_POLL_TIMEOUT_POWER_SEC) == AHCI_CMD_OK) {
        AHCI_DBG((AHCI_PFX "Port %lu.%lu: standby for shutdown\n", Port, (ULONG)dev->Pmp));
    }
}

/* DATA SET MANAGEMENT TRIM of Count sectors from Lba: one 512-byte block
   of up to 64 ranges per command, through the port's bounce buffer */
static ULONG
AhciTrim(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun, IN AHCI_LBA Lba, IN ULONG Count)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    PULONG range;
    ATA_REQUEST req;
    ULONG n, piece, result;

    while (Count > 0) {
        range = (PULONG)pi->BounceBuffer;
        ZeroMemoryBytes(range, AHCI_BOUNCE_SIZE);
        for (n = 0; n < IDE_DSM_RANGES_PER_BLOCK && Count > 0; n++) {
            piece = (Count > IDE_DSM_MAX_RANGE) ? IDE_DSM_MAX_RANGE : Count;
            range[n * 2] = Lba.Low;
            range[n * 2 + 1] = (Lba.High & 0xFFFF) | (piece << 16);
            AhciLbaAdd(&Lba, piece);
            Count -= piece;
        }
        ZeroMemoryBytes(&req, sizeof(req));
        req.Pmp = AHCI_DEVICE_OF(HwInit, Port, Lun)->Pmp;
        req.Command = IDE_COMMAND_DATA_SET_MANAGEMENT;
        req.Features = IDE_DSM_TRIM;
        req.Is48Bit = TRUE;
        req.SectorCount = 1;
        req.UseBounce = TRUE;
        req.DataBufferLen = AHCI_BOUNCE_SIZE;
        req.IsWrite = TRUE;
        req.TimeoutSec = AHCI_POLL_TIMEOUT_POWER_SEC;
        result = AhciRunCommand(HwInit, Port, NULL, &req);
        if (result != AHCI_CMD_OK) return result;
    }
    return AHCI_CMD_OK;
}

/* TRIM mode: the write's data is the pattern, repeated from the first byte */
static BOOLEAN
AhciIsTrimPattern(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR Data, IN ULONG Length)
{
    ULONG i, j;

    if (Data == NULL || Length < AHCI_TRIM_PATTERN_SIZE) return FALSE;
    for (i = 0; i < Length; i += AHCI_TRIM_PATTERN_SIZE) {
        for (j = 0; j < AHCI_TRIM_PATTERN_SIZE && i + j < Length; j += 4) {
            if (*(ULONG UNALIGNED *)(Data + i + j) != *(ULONG UNALIGNED *)(&HwInit->TrimPattern[j])) return FALSE;
        }
    }
    return TRUE;
}

/* UNMAP: the block descriptors become DATA SET MANAGEMENT TRIM ranges */
static VOID
AhciAtaUnmap(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Srb->Lun);
    PUCHAR param = (PUCHAR)Srb->DataBuffer;
    ULONG listLen = ((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8], descLen, n, i, count, result;
    AHCI_LBA lba;

    if (!dev->Trim || !HwInit->TrimAllowed) {
        AhciFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_INVALID_OPCODE, 0);
        return;
    }
    if (listLen > Srb->DataTransferLength) listLen = Srb->DataTransferLength;
    if (listLen == 0 || param == NULL) {
        AhciGood(Srb);
        return;
    }
    if (listLen < 8) {
        AhciFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_PARAM_LIST_LENGTH, 0);
        return;
    }
    descLen = ((ULONG)param[2] << 8) | param[3];
    if (descLen > listLen - 8) descLen = listLen - 8;
    n = descLen / 16;
    if (n > AHCI_UNMAP_MAX_DESCRIPTORS) {
        AhciFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_INVALID_PARAM_FIELD, 0);
        return;
    }
    for (i = 0; i < n; i++) {
        lba = AhciLbaFromBigEndian(param + 8 + i * 16, 8);
        count = AhciBigEndian32(param + 8 + i * 16 + 8);
        if (count != 0 && (count > AHCI_UNMAP_MAX_BLOCKS || !AhciLbaRangeValid(dev, lba, count))) {
            AhciFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_LBA_OUT_OF_RANGE, 0);
            return;
        }
    }
    for (i = 0; i < n; i++) {
        lba = AhciLbaFromBigEndian(param + 8 + i * 16, 8);
        count = AhciBigEndian32(param + 8 + i * 16 + 8);
        if (count == 0) continue;
        result = AhciTrim(HwInit, Port, Srb->Lun, lba, count);
        if (result != AHCI_CMD_OK) {
            AhciCompleteResult(HwInit, Port, Srb, result);
            return;
        }
    }
    AhciGood(Srb);
}

/* SMART: threshold state and temperature (attribute C2h, else BEh).
   FALSE when the drive has no SMART or it failed. */
static BOOLEAN
AhciSmartHealth(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun, OUT PBOOLEAN Exceeded,
                OUT PUCHAR Temperature)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Lun);
    PUCHAR data = pi->BounceBuffer;
    ATA_REQUEST req;
    ULONG i;

    *Exceeded = FALSE;
    *Temperature = 0xFF;
    if (!dev->Smart) return FALSE;

    ZeroMemoryBytes(&req, sizeof(req));
    req.Pmp = dev->Pmp;
    req.Command = IDE_COMMAND_SMART;
    req.Features = IDE_SMART_RETURN_STATUS_FEATURE;
    req.Lba.Low = IDE_SMART_LBA_SIGNATURE;
    if (AhciRunCommand(HwInit, Port, NULL, &req) != AHCI_CMD_OK) return FALSE;
    *Exceeded = (BOOLEAN)(pi->Result.Lba1 == 0xF4 && pi->Result.Lba2 == 0x2C);

    ZeroMemoryBytes(&req, sizeof(req));
    ZeroMemoryBytes(data, AHCI_BOUNCE_SIZE);
    req.Pmp = dev->Pmp;
    req.Command = IDE_COMMAND_SMART;
    req.Features = IDE_SMART_READ_DATA_FEATURE;
    req.SectorCount = 1;
    req.Lba.Low = IDE_SMART_LBA_SIGNATURE;
    req.UseBounce = TRUE;
    req.DataBufferLen = AHCI_BOUNCE_SIZE;
    if (AhciRunCommand(HwInit, Port, NULL, &req) == AHCI_CMD_OK) {
        for (i = 0; i < 30; i++) {
            PUCHAR a = data + 2 + i * 12;
            if (a[0] == 0xC2 || (a[0] == 0xBE && *Temperature == 0xFF)) *Temperature = a[5];
        }
    }
    return TRUE;
}

/* LOG SENSE: supported pages, temperature, informational exceptions */
static VOID
AhciAtaLogSense(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Srb->Lun);
    UCHAR pageCode = (UCHAR)(Srb->Cdb[2] & 0x3F), buf[20], temperature;
    ULONG allocLen = ((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8], len;
    BOOLEAN exceeded;

    ZeroMemoryBytes(buf, sizeof(buf));
    buf[0] = pageCode;
    switch (pageCode) {
    case AHCI_LOG_PAGE_SUPPORTED:
        buf[4] = AHCI_LOG_PAGE_SUPPORTED;
        len = 5;
        if (dev->Smart) buf[len++] = AHCI_LOG_PAGE_TEMPERATURE;
        buf[len++] = AHCI_LOG_PAGE_INFO_EXCEPTIONS;
        break;
    case AHCI_LOG_PAGE_TEMPERATURE:
        if (!AhciSmartHealth(HwInit, Port, Srb->Lun, &exceeded, &temperature)) {
            AhciInvalidField(dev, Srb);
            return;
        }
        /* 0000h temperature, 0001h reference temperature (not known) */
        buf[6] = 0x03; buf[7] = 2; buf[9] = temperature;
        buf[11] = 0x01; buf[12] = 0x03; buf[13] = 2; buf[15] = 0xFF;
        len = 16;
        break;
    case AHCI_LOG_PAGE_INFO_EXCEPTIONS:
        AhciSmartHealth(HwInit, Port, Srb->Lun, &exceeded, &temperature);
        buf[6] = 0x03;
        buf[7] = 3;
        if (exceeded) buf[8] = AHCI_ASC_FAILURE_PREDICTION;
        buf[10] = temperature;
        len = 11;
        break;
    default:
        AhciInvalidField(dev, Srb);
        return;
    }
    buf[2] = (UCHAR)((len - 4) >> 8);
    buf[3] = (UCHAR)(len - 4);
    if (len > allocLen) len = allocLen;
    AhciCopyToSrb(Srb, buf, len);
    AhciGood(Srb);
}

static VOID
AhciAtaReadWrite(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN AHCI_LBA Lba,
    IN ULONG Count,
    IN BOOLEAN IsWrite,
    IN BOOLEAN Fua
)
{
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Srb->Lun);
    ATA_REQUEST req;
    ULONG result;
    UCHAR status;

    /* A zero transfer length in READ/WRITE(10)/(16) moves nothing; ATA
       would read 0 as 256/65536 sectors, so never send it. */
    if (Count == 0) {
        AhciGood(Srb);
        return;
    }

    if (!dev->LbaSupported || !AhciLbaRangeValid(dev, Lba, Count)) {
        AHCI_DBG((AHCI_PFX "Port %lu: LBA %08lX:%08lX +%lu out of range\n", Port, Lba.High, Lba.Low, Count));
        AhciFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_LBA_OUT_OF_RANGE, 0);
        return;
    }

    if (Count > (Srb->DataTransferLength / dev->SectorSize)) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return;
    }
    if (!IsWrite) Fua = FALSE;

    /* TRIM mode: a write of the pattern becomes a TRIM of its sectors */
    if (IsWrite && HwInit->TrimMode && dev->Trim && HwInit->TrimAllowed &&
        AhciIsTrimPattern(HwInit, (PUCHAR)Srb->DataBuffer, Count * dev->SectorSize)) {
        AhciCompleteResult(HwInit, Port, Srb, AhciTrim(HwInit, Port, Srb->Lun, Lba, Count));
        return;
    }

    /* NCQ: completes in the interrupt */
    if (HwInit->AsyncMode && dev->Ncq && Count <= ATA_LBA48_MAX_SECTORS && !HwInit->DumpMode) {
        status = AhciIssueNcq(HwInit, Port, Srb, Lba, Count, IsWrite, Fua);
        if (status != SRB_STATUS_PENDING) Srb->SrbStatus = status;
        return;
    }

    ZeroMemoryBytes(&req, sizeof(req));
    req.Pmp = dev->Pmp;
    req.DataBuffer = Srb->DataBuffer;
    req.DataBufferLen = Count * dev->SectorSize;
    req.IsWrite = IsWrite;
    req.Lba = Lba;
    req.SectorCount = Count;

    if (Fua && dev->Fua && Count <= ATA_LBA48_MAX_SECTORS) {
        req.Is48Bit = TRUE;
        req.Command = IDE_COMMAND_WRITE_DMA_FUA_EXT;
        Fua = FALSE;
    } else if (AhciLbaFits28(Lba, Count)) {
        /* 28-bit when possible, EXT only on LBA48 devices when needed */
        req.Is48Bit = FALSE;
        req.Command = IsWrite ? IDE_COMMAND_WRITE_DMA : IDE_COMMAND_READ_DMA;
    } else if (dev->Lba48 && Count <= ATA_LBA48_MAX_SECTORS) {
        req.Is48Bit = TRUE;
        req.Command = IsWrite ? IDE_COMMAND_WRITE_DMA_EXT : IDE_COMMAND_READ_DMA_EXT;
    } else {
        /* Cannot happen for a valid range on a 28-bit-only device (its
           capacity is < 2^28) unless the count is out of bounds. */
        AhciInvalidField(dev, Srb);
        return;
    }

    result = AhciRunCommand(HwInit, Port, Srb, &req);
    /* FUA without WRITE DMA FUA EXT: write, then flush */
    if (result == AHCI_CMD_OK && Fua && !AhciFlushDevice(HwInit, Port, Srb->Lun)) {
        result = AHCI_CMD_DEVICE_ERROR;
    }
    AhciCompleteResult(HwInit, Port, Srb, result);
}

/* VERIFY(10/12/16) without BYTCHK: READ VERIFY SECTORS, in pieces */
static VOID
AhciAtaVerify(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb,
              IN AHCI_LBA Lba, IN ULONG Count)
{
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Srb->Lun);
    ATA_REQUEST req;
    ULONG chunk, result = AHCI_CMD_OK;

    if (Srb->Cdb[1] & 0x02) {                   /* BYTCHK: compare data */
        AhciInvalidField(dev, Srb);
        return;
    }
    if (Count == 0) {
        AhciGood(Srb);
        return;
    }
    if (!dev->LbaSupported || !AhciLbaRangeValid(dev, Lba, Count)) {
        AhciFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_LBA_OUT_OF_RANGE, 0);
        return;
    }

    while (Count > 0 && result == AHCI_CMD_OK) {
        ZeroMemoryBytes(&req, sizeof(req));
        req.Pmp = dev->Pmp;
        req.Lba = Lba;
        if (dev->Lba48) {
            chunk = (Count > ATA_LBA48_MAX_SECTORS) ? ATA_LBA48_MAX_SECTORS : Count;
            req.Is48Bit = TRUE;
            req.Command = IDE_COMMAND_READ_VERIFY_EXT;
        } else {
            chunk = (Count > ATA_LBA28_MAX_SECTORS) ? ATA_LBA28_MAX_SECTORS : Count;
            req.Command = IDE_COMMAND_READ_VERIFY;
        }
        req.SectorCount = chunk;
        result = AhciRunCommand(HwInit, Port, Srb, &req);
        AhciLbaAdd(&Lba, chunk);
        Count -= chunk;
    }
    AhciCompleteResult(HwInit, Port, Srb, result);
}

/* START STOP UNIT: power conditions as SAT maps them */
static VOID
AhciAtaStartStop(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR pc = (UCHAR)(Srb->Cdb[4] >> 4);
    BOOLEAN start = (BOOLEAN)((Srb->Cdb[4] & 0x01) != 0);
    UCHAR command = 0;
    ULONG result = AHCI_CMD_OK;

    switch (pc) {
    case 0x0:                                   /* START bit decides */
        if (start) {
            command = IDE_COMMAND_IDLE_IMMEDIATE;
        } else {
            AhciFlushDevice(HwInit, Port, Srb->Lun);
            command = IDE_COMMAND_STANDBY_IMMEDIATE;
        }
        break;
    case 0x1:                                   /* active */
    case 0x2:                                   /* idle */
    case 0xA:                                   /* force idle */
        command = IDE_COMMAND_IDLE_IMMEDIATE;
        break;
    case 0x3:                                   /* standby */
    case 0xB:                                   /* force standby */
        AhciFlushDevice(HwInit, Port, Srb->Lun);
        command = IDE_COMMAND_STANDBY_IMMEDIATE;
        break;
    default:                                    /* LU control and the rest */
        break;
    }

    if (command != 0) {
        result = AhciRunSimple(HwInit, Port, Srb->Lun, command, 0, AHCI_POLL_TIMEOUT_POWER_SEC);
    }
    AhciCompleteResult(HwInit, Port, Srb, result);
}

/* ------------------------------------------------------------------ */
/* SAT ATA PASS-THROUGH (12) / (16)                                    */
/* ------------------------------------------------------------------ */

#define SAT_PROT_NON_DATA       3
#define SAT_PROT_PIO_IN         4
#define SAT_PROT_PIO_OUT        5
#define SAT_PROT_DMA            6
#define SAT_PROT_RETURN_INFO    15

/* Descriptor sense with the ATA Status Return descriptor (SAT 12.2.5) */
static VOID
AhciPassThroughSense(IN PAHCI_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb, IN PAHCI_ATA_RESULT R,
                     IN BOOLEAN Extend, IN BOOLEAN Error)
{
    UCHAR s[22];

    ZeroMemoryBytes(s, sizeof(s));
    s[0] = 0x72;
    s[1] = Error ? SCSI_SENSE_ABORTED_COMMAND : SCSI_SENSE_RECOVERED_ERROR;
    s[2] = 0x00;
    s[3] = AHCI_ASCQ_ATA_INFO_AVAILABLE;
    s[7] = 14;
    s[8] = 0x09;
    s[9] = 0x0C;
    s[10] = (UCHAR)(Extend ? 1 : 0);
    s[11] = R->Error;
    s[12] = R->CountHigh;
    s[13] = R->CountLow;
    s[14] = R->Lba3;
    s[15] = R->Lba0;
    s[16] = R->Lba4;
    s[17] = R->Lba1;
    s[18] = R->Lba5;
    s[19] = R->Lba2;
    s[20] = R->Device;
    s[21] = R->Status;
    AhciFailWithRawSense(Dev, Srb, s, sizeof(s));
}

static VOID
AhciAtaPassThrough(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Srb->Lun);
    PUCHAR cdb = Srb->Cdb;
    BOOLEAN sixteen = (BOOLEAN)(cdb[0] == AHCI_SCSIOP_ATA_PASSTHROUGH16);
    UCHAR protocol = (UCHAR)((cdb[1] >> 1) & 0x0F);
    BOOLEAN extend = (BOOLEAN)(sixteen && (cdb[1] & 0x01));
    BOOLEAN ckCond = (BOOLEAN)((cdb[2] & 0x20) != 0);
    BOOLEAN toDevice = (BOOLEAN)((cdb[2] & 0x08) == 0);
    ATA_REQUEST req;
    ULONG result;

    if (protocol == SAT_PROT_RETURN_INFO) {
        AhciPassThroughSense(dev, Srb, &pi->Result, extend, FALSE);
        return;
    }
    if (protocol != SAT_PROT_NON_DATA && protocol != SAT_PROT_PIO_IN &&
        protocol != SAT_PROT_PIO_OUT && protocol != SAT_PROT_DMA) {
        AhciInvalidField(dev, Srb);
        return;
    }

    ZeroMemoryBytes(&req, sizeof(req));
    req.Pmp = dev->Pmp;
    req.RawDevice = TRUE;
    req.Is48Bit = extend;
    if (sixteen) {
        req.Features = (USHORT)(cdb[4] | (extend ? ((USHORT)cdb[3] << 8) : 0));
        req.SectorCount = cdb[6] | (extend ? ((ULONG)cdb[5] << 8) : 0);
        req.Lba.Low = (ULONG)cdb[8] | ((ULONG)cdb[10] << 8) | ((ULONG)cdb[12] << 16);
        if (extend) {
            req.Lba.Low |= (ULONG)cdb[7] << 24;
            req.Lba.High = (ULONG)cdb[9] | ((ULONG)cdb[11] << 8);
        }
        req.Device = cdb[13];
        req.Command = cdb[14];
    } else {
        req.Features = cdb[3];
        req.SectorCount = cdb[4];
        req.Lba.Low = (ULONG)cdb[5] | ((ULONG)cdb[6] << 8) | ((ULONG)cdb[7] << 16);
        req.Device = cdb[8];
        req.Command = cdb[9];
    }
    /* SATA has one device per link: DEV must be 0 */
    req.Device &= (UCHAR)~0x10;

    if (protocol != SAT_PROT_NON_DATA) {
        if (Srb->DataBuffer == NULL || Srb->DataTransferLength == 0) {
            AhciInvalidField(dev, Srb);
            return;
        }
        req.DataBuffer = Srb->DataBuffer;
        req.DataBufferLen = Srb->DataTransferLength;
        req.IsWrite = (BOOLEAN)(protocol == SAT_PROT_PIO_OUT || (protocol == SAT_PROT_DMA && toDevice));
    }

    result = AhciRunCommand(HwInit, Port, Srb, &req);
    if (result == AHCI_CMD_BAD_BUFFER) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
    } else if (result != AHCI_CMD_OK || ckCond) {
        if (result != AHCI_CMD_OK) {
            pi->Result.Status = (UCHAR)(dev->LastTfd | TFD_STS_ERR);
            pi->Result.Error = (UCHAR)(dev->LastTfd >> TFD_ERR_SHIFT);
        }
        AhciPassThroughSense(dev, Srb, &pi->Result, extend, (BOOLEAN)(result != AHCI_CMD_OK));
    } else {
        AhciGood(Srb);
    }
}

static VOID
AhciProcessAtaSrb(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Srb->Lun);
    PUCHAR cdb = Srb->Cdb;
    AHCI_LBA lba;
    ULONG count;
    BOOLEAN fua = (BOOLEAN)((cdb[1] & 0x08) != 0);

    switch (cdb[0]) {
    case SCSIOP_INQUIRY:
        AhciHandleInquiry(HwInit, dev, Srb);
        return;

    case SCSIOP_REQUEST_SENSE:
        AhciHandleRequestSenseAta(dev, Srb);
        return;

    case AHCI_SCSIOP_REPORT_LUNS:
        AhciHandleReportLuns(pi, Srb);
        return;

    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_MEDIUM_REMOVAL:
        AhciGood(Srb);
        return;

    case SCSIOP_SYNCHRONIZE_CACHE:
    case AHCI_SCSIOP_SYNC_CACHE16:
        AhciCompleteResult(HwInit, Port, Srb,
                           AhciFlushDevice(HwInit, Port, Srb->Lun) ? AHCI_CMD_OK : AHCI_CMD_DEVICE_ERROR);
        return;

    case AHCI_SCSIOP_START_STOP_UNIT:
        AhciAtaStartStop(HwInit, Port, Srb);
        return;

    case SCSIOP_READ_CAPACITY:
        AhciHandleReadCapacity10(HwInit, dev, Srb);
        return;

    case AHCI_SCSIOP_SERVICE_ACTION_IN16:
        if ((cdb[1] & 0x1F) == AHCI_SA_READ_CAPACITY16) {
            AhciHandleReadCapacity16(HwInit, dev, Srb);
            return;
        }
        break;

    case SCSIOP_MODE_SENSE:
    case SCSIOP_MODE_SENSE10:
        AhciHandleModeSenseAta(HwInit, dev, Srb);
        return;

    case AHCI_SCSIOP_MODE_SELECT6:
    case AHCI_SCSIOP_MODE_SELECT10:
        AhciHandleModeSelectAta(HwInit, Port, Srb->Lun, Srb);
        return;

    case AHCI_SCSIOP_ATA_PASSTHROUGH12:
    case AHCI_SCSIOP_ATA_PASSTHROUGH16:
        AhciAtaPassThrough(HwInit, Port, Srb);
        return;

    case SCSIOP_READ6:
    case SCSIOP_WRITE6:
        lba.High = 0;
        lba.Low = ((ULONG)(cdb[1] & 0x1F) << 16) | ((ULONG)cdb[2] << 8) | cdb[3];
        count = cdb[4] ? cdb[4] : 256;
        AhciAtaReadWrite(HwInit, Port, Srb, lba, count, (BOOLEAN)(cdb[0] == SCSIOP_WRITE6), FALSE);
        return;

    case SCSIOP_READ:
    case SCSIOP_WRITE:
        lba = AhciLbaFromBigEndian(&cdb[2], 4);
        count = ((ULONG)cdb[7] << 8) | cdb[8];
        AhciAtaReadWrite(HwInit, Port, Srb, lba, count, (BOOLEAN)(cdb[0] == SCSIOP_WRITE), fua);
        return;

    case AHCI_SCSIOP_READ12:
    case AHCI_SCSIOP_WRITE12:
        lba = AhciLbaFromBigEndian(&cdb[2], 4);
        count = AhciBigEndian32(&cdb[6]);
        AhciAtaReadWrite(HwInit, Port, Srb, lba, count, (BOOLEAN)(cdb[0] == AHCI_SCSIOP_WRITE12), fua);
        return;

    case AHCI_SCSIOP_READ16:
    case AHCI_SCSIOP_WRITE16:
        lba = AhciLbaFromBigEndian(&cdb[2], 8);
        count = AhciBigEndian32(&cdb[10]);
        AhciAtaReadWrite(HwInit, Port, Srb, lba, count, (BOOLEAN)(cdb[0] == AHCI_SCSIOP_WRITE16), fua);
        return;

    case SCSIOP_VERIFY:
        AhciAtaVerify(HwInit, Port, Srb, AhciLbaFromBigEndian(&cdb[2], 4), ((ULONG)cdb[7] << 8) | cdb[8]);
        return;

    case AHCI_SCSIOP_VERIFY12:
        AhciAtaVerify(HwInit, Port, Srb, AhciLbaFromBigEndian(&cdb[2], 4), AhciBigEndian32(&cdb[6]));
        return;

    case AHCI_SCSIOP_VERIFY16:
        AhciAtaVerify(HwInit, Port, Srb, AhciLbaFromBigEndian(&cdb[2], 8), AhciBigEndian32(&cdb[10]));
        return;

    case AHCI_SCSIOP_UNMAP:
        AhciAtaUnmap(HwInit, Port, Srb);
        return;

    case AHCI_SCSIOP_LOG_SENSE:
        AhciAtaLogSense(HwInit, Port, Srb);
        return;
    }

    AhciFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, AHCI_ASC_INVALID_OPCODE, 0);
}

/* ------------------------------------------------------------------ */
/* ATAPI                                                               */
/* ------------------------------------------------------------------ */

/* ATAPI command with data in the port's bounce buffer */
static ULONG
AhciAtapiBounceCommand(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port,
    IN ULONG Lun,
    IN PUCHAR Cdb,
    IN ULONG DataLength
)
{
    ATA_REQUEST req;

    ZeroMemoryBytes(&req, sizeof(req));
    req.Pmp = AHCI_DEVICE_OF(HwInit, Port, Lun)->Pmp;
    req.IsAtapi = TRUE;
    req.UseBounce = TRUE;
    req.DataBufferLen = DataLength;
    req.Cdb = Cdb;
    req.CdbLength = 12;

    ZeroMemoryBytes(HwInit->Ports[Port].BounceBuffer, AHCI_BOUNCE_SIZE);
    return AhciRunCommand(HwInit, Port, NULL, &req);
}

/* REQUEST SENSE sent to the drive; returns the sense length obtained
   (0 on failure). Data is left in the port's bounce buffer. */
static ULONG
AhciAtapiFetchSense(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun, IN ULONG AllocLen)
{
    UCHAR cdb[12];

    if (AllocLen == 0 || AllocLen > 252) AllocLen = sizeof(SENSE_DATA);
    AllocLen = (AllocLen + 1) & ~1UL;           /* PRD byte counts are even */

    ZeroMemoryBytes(cdb, sizeof(cdb));
    cdb[0] = SCSIOP_REQUEST_SENSE;
    cdb[4] = (UCHAR)AllocLen;

    if (AhciAtapiBounceCommand(HwInit, Port, Lun, cdb, AllocLen) != AHCI_CMD_OK) return 0;
    return AllocLen;
}

static VOID
AhciAtapiFailed(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    Srb->SrbStatus = SRB_STATUS_ERROR;
    Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;

#if AHCI_AUTOSENSE
    if (Srb->SenseInfoBuffer != NULL && Srb->SenseInfoBufferLength != 0 &&
        !(Srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE)) {
        PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Srb->Lun);
        ULONG tfd = dev->LastTfd;
        ULONG n;

        /* Real auto-sense: ask the drive, like a SCSI HBA would */
        n = AhciAtapiFetchSense(HwInit, Port, Srb->Lun, sizeof(SENSE_DATA));
        if (n != 0) {
            AhciFailWithRawSense(dev, Srb, HwInit->Ports[Port].BounceBuffer, n);
        } else if (tfd & TFD_STS_ERR) {
            /* Drive would not answer: at least pass on its sense key */
            AhciFailWithSense(dev, Srb, (UCHAR)ATAPI_ERROR_SENSE_KEY(tfd >> TFD_ERR_SHIFT), 0, 0);
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
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Srb->Lun);
    ULONG allocLen = Srb->Cdb[4];
    ULONG n;
    SENSE_DATA empty;

    /* Sense the driver itself produced (e.g. unsupported VPD page) */
    if (dev->SensePending) {
        AhciHandleRequestSenseAta(dev, Srb);
        return;
    }

    if (allocLen > Srb->DataTransferLength) allocLen = Srb->DataTransferLength;

    n = AhciAtapiFetchSense(HwInit, Port, Srb->Lun, allocLen);
    if (n != 0) {
        AhciCopyToSrb(Srb, HwInit->Ports[Port].BounceBuffer, n);
    } else {
        /* The drive gave us nothing: an empty NO SENSE is the honest answer */
        AhciBuildSense(&empty, SCSI_SENSE_NO_SENSE, 0, 0);
        AhciCopyToSrb(Srb, &empty, sizeof(empty));
    }
    AhciGood(Srb);
}

/* ATAPI has no MODE SENSE(6): send (10), convert the header like atapi.sys */
static VOID
AhciAtapiModeSense6(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PUCHAR bounce = HwInit->Ports[Port].BounceBuffer;
    UCHAR cdb[12];
    UCHAR hdr6[4];
    ULONG xferLen, total, blockDesc, i;

    /* The same request with 4 more bytes for the longer header; DBD, page
       control and page code as given */
    xferLen = (ULONG)Srb->Cdb[4] + 4;
    if (xferLen < 8) xferLen = 8;               /* at least the whole header */
    if (xferLen > AHCI_BOUNCE_SIZE) xferLen = AHCI_BOUNCE_SIZE;
    xferLen = (xferLen + 1) & ~1UL;             /* PRD byte counts are even */

    ZeroMemoryBytes(cdb, sizeof(cdb));
    cdb[0] = SCSIOP_MODE_SENSE10;
    cdb[1] = (UCHAR)(Srb->Cdb[1] & 0x08);
    cdb[2] = Srb->Cdb[2];
    cdb[7] = (UCHAR)(xferLen >> 8);
    cdb[8] = (UCHAR)xferLen;

    if (AhciAtapiBounceCommand(HwInit, Port, Srb->Lun, cdb, xferLen) != AHCI_CMD_OK) {
        AhciAtapiFailed(HwInit, Port, Srb);
        return;
    }

    /* Header (10) to header (6): data length less 3, medium type, device
       specific parameter (write protect of a removable disk), block
       descriptor length; the descriptors and pages follow unchanged */
    total = (((ULONG)bounce[0] << 8) | bounce[1]) + 2;     /* all the drive has */
    if (total < 8) total = 8;
    blockDesc = ((ULONG)bounce[6] << 8) | bounce[7];
    hdr6[0] = (UCHAR)((total - 5 > 255) ? 255 : total - 5);
    if (total > xferLen) total = xferLen;                   /* what came back */
    hdr6[1] = bounce[2];
    hdr6[2] = bounce[3];
    hdr6[3] = (UCHAR)((blockDesc > 255) ? 255 : blockDesc);
    for (i = 8; i < total; i++) bounce[i - 4] = bounce[i];
    for (i = 0; i < 4; i++) bounce[i] = hdr6[i];

    AhciCopyToSrb(Srb, bounce, total - 4);
    AhciGood(Srb);
}

static VOID
AhciProcessAtapiSrb(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Srb->Lun);
    ATA_REQUEST req;
    ULONG result;

    /* ATAPI has no READ(6)/WRITE(6) (MMC, SFF-8070i): the same transfer as
       READ(10)/WRITE(10), for a class driver of a removable disk that
       sends the short form */
    if (Srb->Cdb[0] == SCSIOP_READ6 || Srb->Cdb[0] == SCSIOP_WRITE6) {
        PUCHAR c = Srb->Cdb;
        ULONG lba = ((ULONG)(c[1] & 0x1F) << 16) | ((ULONG)c[2] << 8) | c[3];
        ULONG count = (c[4] != 0) ? c[4] : 256;
        UCHAR op = (UCHAR)((c[0] == SCSIOP_READ6) ? SCSIOP_READ : SCSIOP_WRITE);

        ZeroMemoryBytes(c, 10);
        c[0] = op;
        c[2] = (UCHAR)(lba >> 24);
        c[3] = (UCHAR)(lba >> 16);
        c[4] = (UCHAR)(lba >> 8);
        c[5] = (UCHAR)lba;
        c[7] = (UCHAR)(count >> 8);
        c[8] = (UCHAR)count;
        Srb->CdbLength = 10;
    }

    switch (Srb->Cdb[0]) {
    case SCSIOP_INQUIRY:
        AhciHandleInquiry(HwInit, dev, Srb);
        return;

    case SCSIOP_REQUEST_SENSE:
        AhciAtapiRequestSense(HwInit, Port, Srb);
        return;

    case AHCI_SCSIOP_REPORT_LUNS:
        AhciHandleReportLuns(&HwInit->Ports[Port], Srb);
        return;

    case SCSIOP_SYNCHRONIZE_CACHE:
        AhciGood(Srb);
        return;

    case SCSIOP_MODE_SENSE:
        AhciAtapiModeSense6(HwInit, Port, Srb);
        return;
    }

    /* Everything else (TEST UNIT READY, READ(10/12), READ TOC, MODE
       SENSE(10), START STOP UNIT, ...) is a native ATAPI packet. */
    ZeroMemoryBytes(&req, sizeof(req));
    req.Pmp = dev->Pmp;
    req.IsAtapi = TRUE;
    req.DataBuffer = Srb->DataBuffer;
    req.DataBufferLen = (Srb->DataBuffer != NULL) ? Srb->DataTransferLength : 0;
    req.IsWrite = (BOOLEAN)((Srb->SrbFlags & SRB_FLAGS_DATA_OUT) != 0);
    req.Cdb = Srb->Cdb;
    req.CdbLength = Srb->CdbLength;

    result = AhciRunCommand(HwInit, Port, Srb, &req);
    if (result == AHCI_CMD_OK) {
        AhciGood(Srb);
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
    ULONG port = AHCI_PORT_OF(Srb);
    PAHCI_PORT_INFO pi;
    PAHCI_DEVICE dev;

    if (Srb->TargetId >= AHCI_TARGETS_PER_BUS || port >= HwInit->MaxPorts || !HwInit->Ports[port].Implemented ||
        HwInit->Ports[port].DeviceCount == 0) {
        Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        return;
    }
    pi = &HwInit->Ports[port];

    /* After a COMRESET: PM links and device settings first */
    if (pi->NeedsSetup && pi->Outstanding == 0) {
        AHCI_DBG((AHCI_PFX "Port %lu: setting up again after reset\n", port));
        AhciSetupPort(HwInit, port);
    }

    if (Srb->Lun >= pi->DeviceCount || !AHCI_DEVICE_OF(HwInit, port, Srb->Lun)->Present) {
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        return;
    }
    dev = AHCI_DEVICE_OF(HwInit, port, Srb->Lun);

    /* An ATA disk that did not answer IDENTIFY has no known capacity: it
       is not presented at all rather than as an empty disk */
    if (!dev->IsAtapi && !dev->IdentifyValid) {
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        return;
    }

    /* Sense belongs to the command that failed: any other command
       discards it, so a later REQUEST SENSE never reports a stale error. */
    if (Srb->Cdb[0] != SCSIOP_REQUEST_SENSE) {
        dev->SensePending = FALSE;
    }

    if (dev->IsAtapi) {
        AhciProcessAtapiSrb(HwInit, port, Srb);
    } else {
        AhciProcessAtaSrb(HwInit, port, Srb);
    }
}

/* ------------------------------------------------------------------ */
/* Miniport IOCTLs: SMART ("SCSIDISK") and power control ("AHCINT")    */
/* ------------------------------------------------------------------ */

static BOOLEAN
AhciSignatureIs(IN PAHCI_SRB_IO_CONTROL Ctl, IN PCHAR Signature)
{
    ULONG i;
    for (i = 0; i < 8; i++) {
        if (Ctl->Signature[i] != (UCHAR)Signature[i]) return FALSE;
    }
    return TRUE;
}

/* Disk for a SMART request: bDriveNumber is the target (port); behind a
   PM the LUN comes from the SRB */
static PAHCI_DEVICE
AhciSmartDevice(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb, IN UCHAR Drive,
                OUT PULONG Port, OUT PULONG Lun)
{
    ULONG port = Drive, lun = 0;

    if (port >= HwInit->MaxPorts || HwInit->Ports[port].DeviceCount == 0) {
        port = AHCI_PORT_OF(Srb);
    }
    if (port >= HwInit->MaxPorts) return NULL;
    if (port == AHCI_PORT_OF(Srb)) lun = Srb->Lun;
    if (lun >= HwInit->Ports[port].DeviceCount || !AHCI_DEVICE_OF(HwInit, port, lun)->Present) return NULL;
    *Port = port;
    *Lun = lun;
    return AHCI_DEVICE_OF(HwInit, port, lun);
}

static ULONG
AhciSmartCommand(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb, IN PAHCI_SRB_IO_CONTROL Ctl,
                 IN ULONG Available)
{
    PAHCI_SENDCMD_IN in = (PAHCI_SENDCMD_IN)((PUCHAR)Ctl + Ctl->HeaderLength);
    PAHCI_SENDCMD_OUT out = (PAHCI_SENDCMD_OUT)in;
    AHCI_IDEREGS regs;
    ATA_REQUEST req;
    PAHCI_DEVICE dev;
    PAHCI_PORT_INFO pi;
    ULONG port, lun, dataLen = 0, result;
    BOOLEAN dataIn = FALSE, dataOut = FALSE;
    UCHAR drive;

    if (Available < AHCI_SENDCMD_IN_HEADER) return AHCI_SMART_INVALID_BUFFER;
    regs = in->Regs;
    drive = in->DriveNumber;

    dev = AhciSmartDevice(HwInit, Srb, drive, &port, &lun);
    if (dev == NULL) return AHCI_SMART_INVALID_DRIVE;
    pi = &HwInit->Ports[port];

    switch (Ctl->ControlCode) {
    case AHCI_SMART_IDENTIFY:
        regs.Command = dev->IsAtapi ? IDE_COMMAND_IDENTIFY_PACKET : IDE_COMMAND_IDENTIFY_DEVICE;
        dataIn = TRUE;
        dataLen = 512;
        break;
    case AHCI_SMART_READ_ATTRIBS:
    case AHCI_SMART_READ_THRESHOLDS:
        dataIn = TRUE;
        dataLen = 512;
        break;
    case AHCI_SMART_READ_LOG:
        dataIn = TRUE;
        dataLen = (ULONG)(regs.SectorCount ? regs.SectorCount : 1) * 512;
        break;
    case AHCI_SMART_WRITE_LOG:
        dataOut = TRUE;
        dataLen = (ULONG)(regs.SectorCount ? regs.SectorCount : 1) * 512;
        break;
    default:
        break;
    }
    if (dev->IsAtapi && Ctl->ControlCode != AHCI_SMART_IDENTIFY) return AHCI_SMART_INVALID_DRIVE;

    if ((dataIn && Available < AHCI_SENDCMD_OUT_HEADER + dataLen) ||
        (dataOut && Available < AHCI_SENDCMD_IN_HEADER + dataLen)) {
        return AHCI_SMART_INVALID_BUFFER;
    }

    ZeroMemoryBytes(&req, sizeof(req));
    req.Pmp = dev->Pmp;
    req.Command = regs.Command;
    req.Features = regs.Features;
    req.SectorCount = regs.SectorCount;
    req.Lba.Low = (ULONG)regs.SectorNumber | ((ULONG)regs.CylLow << 8) | ((ULONG)regs.CylHigh << 16);
    req.RawDevice = TRUE;
    req.Device = (UCHAR)(regs.DriveHead & 0xE0);
    if (dataIn || dataOut) {
        req.DataBuffer = dataIn ? out->Buffer : in->Buffer;
        req.DataBufferLen = dataLen;
        req.IsWrite = dataOut;
    }

    result = AhciRunCommand(HwInit, port, Srb, &req);

    ZeroMemoryBytes(out, AHCI_SENDCMD_OUT_HEADER);
    if (result != AHCI_CMD_OK) {
        out->DriverError = AHCI_SMART_DRV_ERROR;
        out->IdeError = (UCHAR)(dev->LastTfd >> TFD_ERR_SHIFT);
        return 0;
    }

    if (Ctl->ControlCode == AHCI_SMART_RETURN_STATUS) {
        /* Result registers: CylLow/CylHigh 4Fh/C2h good, F4h/2Ch threshold exceeded */
        PAHCI_IDEREGS r = (PAHCI_IDEREGS)out->Buffer;
        if (Available < AHCI_SENDCMD_OUT_HEADER + sizeof(AHCI_IDEREGS)) return AHCI_SMART_INVALID_BUFFER;
        r->Features = pi->Result.Error;
        r->SectorCount = pi->Result.CountLow;
        r->SectorNumber = pi->Result.Lba0;
        r->CylLow = pi->Result.Lba1;
        r->CylHigh = pi->Result.Lba2;
        r->DriveHead = pi->Result.Device;
        r->Command = pi->Result.Status;
        r->Reserved = 0;
        out->BufferSize = sizeof(AHCI_IDEREGS);
    } else if (dataIn) {
        out->BufferSize = dataLen;
    }
    return 0;
}

static VOID
AhciSmartVersion(IN PHW_DEVICE_EXTENSION HwInit, IN PAHCI_SRB_IO_CONTROL Ctl, IN ULONG Available)
{
    PAHCI_GETVERSION v = (PAHCI_GETVERSION)((PUCHAR)Ctl + Ctl->HeaderLength);
    ULONG port;

    if (Available < sizeof(AHCI_GETVERSION)) {
        Ctl->ReturnCode = AHCI_SMART_INVALID_BUFFER;
        return;
    }
    ZeroMemoryBytes(v, sizeof(AHCI_GETVERSION));
    v->Version = 1;
    v->Revision = 1;
    v->Capabilities = AHCI_SMART_CAP_ATA_ID | AHCI_SMART_CAP_ATAPI_ID | AHCI_SMART_CAP_SMART;
    /* Device map as atapi.sys reports it: bit n ATA, bit n+4 ATAPI */
    for (port = 0; port < 4; port++) {
        PAHCI_PORT_INFO pi = &HwInit->Ports[port];
        if (pi->DeviceCount == 0) continue;
        v->IdeDeviceMap |= (UCHAR)(AHCI_DEVICE_OF(HwInit, port, 0)->IsAtapi ? (0x10 << port) : (0x01 << port));
    }
    Ctl->ReturnCode = 0;
}

VOID
AhciProcessIoControl(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb)
{
    PAHCI_SRB_IO_CONTROL ctl = (PAHCI_SRB_IO_CONTROL)Srb->DataBuffer;
    ULONG available;

    if (ctl == NULL || Srb->DataTransferLength < sizeof(AHCI_SRB_IO_CONTROL) ||
        ctl->HeaderLength < sizeof(AHCI_SRB_IO_CONTROL) ||
        ctl->HeaderLength > Srb->DataTransferLength) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return;
    }
    available = Srb->DataTransferLength - ctl->HeaderLength;
    if (available > ctl->Length) available = ctl->Length;

    if (AhciSignatureIs(ctl, AHCI_SMART_SIGNATURE)) {
        AhciCheckState(HwInit, AHCI_PORT_OF(Srb));
        if (ctl->ControlCode == AHCI_SMART_VERSION) {
            AhciSmartVersion(HwInit, ctl, available);
        } else if (ctl->ControlCode > AHCI_SMART_VERSION && ctl->ControlCode <= AHCI_SMART_WRITE_LOG) {
            ctl->ReturnCode = AhciSmartCommand(HwInit, Srb, ctl, available);
        } else {
            Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
            return;
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        return;
    }

    /* The HBA is not woken up for these: an ACPI driver may query it while
       it is powered down */
    if (AhciSignatureIs(ctl, AHCI_IOCTL_SIGNATURE)) {
        ctl->ReturnCode = 0;
        switch (ctl->ControlCode) {
        case AHCI_IOCTL_GET_INFO:
            if (available >= AHCI_IOCTL_INFO_SIZE_V1) {
                /* Ports 8-31 only into a buffer that has room for them */
                PAHCI_IOCTL_INFO info = (PAHCI_IOCTL_INFO)((PUCHAR)ctl + ctl->HeaderLength);
                ULONG port, size = (available >= sizeof(AHCI_IOCTL_INFO)) ? sizeof(AHCI_IOCTL_INFO) : AHCI_IOCTL_INFO_SIZE_V1;
                ZeroMemoryBytes(info, size);
                info->Version = AHCI_DRIVER_VERSION;
                info->Capabilities = HwInit->HbaCapabilities;
                info->PortsImplemented = HwInit->PortsImplemented;
                info->Flags = (HwInit->AsyncMode ? AHCI_INFO_ASYNC : 0) |
                              (HwInit->Stopped ? AHCI_INFO_POWERED_DOWN : 0) |
                              (HwInit->TrimMode ? AHCI_INFO_TRIM_MODE : 0);
                info->SlotsPerPort = HwInit->Slots;
                for (port = 0; port < 8; port++) {
                    info->DeviceCount[port] = HwInit->Ports[port].DeviceCount;
                }
                for (port = 8; size == sizeof(AHCI_IOCTL_INFO) && port < HwInit->MaxPorts; port++) {
                    info->DeviceCountHigh[port - 8] = HwInit->Ports[port].DeviceCount;
                }
            } else {
                ctl->ReturnCode = 1;
            }
            break;
        case AHCI_IOCTL_POWER_DOWN:
            AhciPowerDown(HwInit);
            break;
        case AHCI_IOCTL_POWER_UP:
            if (HwInit->Stopped) AhciReinitialize(HwInit);
            break;
        case AHCI_IOCTL_TRIM_MODE_ON:
            if (available < AHCI_TRIM_PATTERN_SIZE || !HwInit->TrimAllowed || HwInit->TrimPattern == NULL) {
                ctl->ReturnCode = 1;
                break;
            }
            CopyMemoryBytes(HwInit->TrimPattern, (PUCHAR)ctl + ctl->HeaderLength, AHCI_TRIM_PATTERN_SIZE);
            HwInit->TrimMode = TRUE;
            break;
        case AHCI_IOCTL_TRIM_MODE_OFF:
            HwInit->TrimMode = FALSE;
            break;
        default:
            Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
            return;
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        return;
    }

    Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
}
