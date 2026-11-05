#include "ahcint.h"

/* ahcicmd.c - command engine: Fast Polling, NCQ, port reset, port multiplier access */

/* ------------------------------------------------------------------ */
/* Port engine control                                                 */
/* ------------------------------------------------------------------ */

VOID
AhciStopPortEngines(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR PortBase)
{
    ULONG cmd;
    ULONG timeout;

    cmd = AHCI_READ_REG(PortBase, AHCI_PORT_CMD);
    if (cmd & AHCI_PORT_CMD_ST) {
        AHCI_WRITE_REG(PortBase, AHCI_PORT_CMD, cmd & ~AHCI_PORT_CMD_ST);

        timeout = 50000;
        while ((AHCI_READ_REG(PortBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) && --timeout) {
            ScsiPortStallExecution(10);
        }

        if ((AHCI_READ_REG(PortBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) &&
            (HwInit->HbaCapabilities & AHCI_CAP_SCLO)) {
            cmd = AHCI_READ_REG(PortBase, AHCI_PORT_CMD);
            AHCI_WRITE_REG(PortBase, AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_CLO);
            timeout = 5000;
            while ((AHCI_READ_REG(PortBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CLO) && --timeout) {
                ScsiPortStallExecution(10);
            }
        }
    }

    cmd = AHCI_READ_REG(PortBase, AHCI_PORT_CMD);
    if (cmd & AHCI_PORT_CMD_FRE) {
        AHCI_WRITE_REG(PortBase, AHCI_PORT_CMD, cmd & ~AHCI_PORT_CMD_FRE);

        timeout = 50000;
        while ((AHCI_READ_REG(PortBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_FR) && --timeout) {
            ScsiPortStallExecution(10);
        }
    }
}

/* Error recovery: stop the engine (clears PxCI/PxSACT), clear status,
   start FIS receive and the command engine again. COMRESET when asked,
   or when the engine will not stop or the device stays busy. Returns
   TRUE when a COMRESET was issued (device settings may be lost). */
BOOLEAN
AhciRestartPort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN BOOLEAN ForceComReset)
{
    PUCHAR portBase = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    BOOLEAN comReset = FALSE;
    ULONG timeout;

    AhciStopPortEngines(HwInit, portBase);

    if (ForceComReset ||
        (AHCI_READ_REG(portBase, AHCI_PORT_CMD) & (AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR)) ||
        (AHCI_READ_REG(portBase, AHCI_PORT_TFD) & (TFD_STS_BSY | TFD_STS_DRQ))) {
        AHCI_DBG((AHCI_PFX "Port %lu: COMRESET\n", Port));
        AHCI_WRITE_REG(portBase, AHCI_PORT_SCTL, 0x301);
        ScsiPortStallExecution(1000);
        AHCI_WRITE_REG(portBase, AHCI_PORT_SCTL, 0x300);
        timeout = 1000;
        while ((AHCI_READ_REG(portBase, AHCI_PORT_SSTS) & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_PHY && --timeout) {
            ScsiPortStallExecution(1000);
        }
        comReset = TRUE;
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);

    /* ST only once the device has dropped BSY/DRQ (AHCI 10.3.1) */
    timeout = 2000;
    while ((AHCI_READ_REG(portBase, AHCI_PORT_TFD) & (TFD_STS_BSY | TFD_STS_DRQ)) && --timeout) {
        ScsiPortStallExecution(1000);
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << Port));

    pi->Outstanding = 0;
    if (comReset) {
        /* PM links and device settings are set up again before the next
           command (see AhciSatlProcessSrb). */
        pi->NeedsSetup = TRUE;
    }
    return comReset;
}

/* ------------------------------------------------------------------ */
/* Command building                                                    */
/* ------------------------------------------------------------------ */

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

    while (left > 0 && index < HwInit->PrdtEntries) {
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

static VOID
AhciFillFis(OUT PFIS_REG_H2D Fis, IN PATA_REQUEST Req)
{
    ZeroMemoryBytes(Fis, sizeof(FIS_REG_H2D));
    Fis->FisType = FIS_TYPE_REG_H2D;
    Fis->PmPortControl = (UCHAR)(FIS_H2D_COMMAND | (Req->Pmp & 0x0F));

    if (Req->IsAtapi) {
        ULONG byteCount = (Req->DataBufferLen > 0xFFFE) ? 0xFFFE : Req->DataBufferLen;

        Fis->Command = IDE_COMMAND_PACKET;
#ifdef AHCI_NT4
        /* NT4/9x: DMA bit on every packet */
        Fis->FeaturesLow = 0x01;
#else
        Fis->FeaturesLow = (Req->DataBufferLen > 0) ? 0x01 : 0x00;      /* DMA */
#endif
        Fis->Lba1 = (UCHAR)(byteCount & 0xFF);
        Fis->Lba2 = (UCHAR)((byteCount >> 8) & 0xFF);
        return;
    }

    Fis->Command = Req->Command;
    Fis->FeaturesLow = (UCHAR)Req->Features;
    Fis->Lba0 = (UCHAR)(Req->Lba.Low);
    Fis->Lba1 = (UCHAR)(Req->Lba.Low >> 8);
    Fis->Lba2 = (UCHAR)(Req->Lba.Low >> 16);
    /* 256 / 65536 sectors are encoded as 0 */
    Fis->SectorCountLow = (UCHAR)(Req->SectorCount);
    if (Req->Is48Bit) {
        Fis->Device = ATA_DEV_LBA;
        Fis->Lba3 = (UCHAR)(Req->Lba.Low >> 24);
        Fis->Lba4 = (UCHAR)(Req->Lba.High);
        Fis->Lba5 = (UCHAR)(Req->Lba.High >> 8);
        Fis->FeaturesHigh = (UCHAR)(Req->Features >> 8);
        Fis->SectorCountHigh = (UCHAR)(Req->SectorCount >> 8);
    } else {
        Fis->Device = (UCHAR)(ATA_DEV_LBA | ((Req->Lba.Low >> 24) & 0x0F));
    }
    if (Req->RawDevice) {
        Fis->Device = Req->Device;
    }
}

/* Registers the device returned: from the PIO Setup FIS for PIO data-in,
   otherwise from the D2H Register FIS. */
VOID
AhciReadResult(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN BOOLEAN PioSetup)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    PUCHAR f = pi->ReceivedFis + (PioSetup ? AHCI_RFIS_PIO_SETUP : AHCI_RFIS_D2H);
    PAHCI_ATA_RESULT r = &pi->Result;

    r->Status = PioSetup ? f[15] : f[2];
    r->Error = f[3];
    r->Lba0 = f[4];
    r->Lba1 = f[5];
    r->Lba2 = f[6];
    r->Device = f[7];
    r->Lba3 = f[8];
    r->Lba4 = f[9];
    r->Lba5 = f[10];
    r->CountLow = f[12];
    r->CountHigh = f[13];
}

/* ------------------------------------------------------------------ */
/* Commands that are not queued: by interrupt, or Fast Polling          */
/* ------------------------------------------------------------------ */

/* Issue one command on slot 0. Queued commands on the port, and a command
   of the interrupt path still running, are completed first: ATA forbids
   mixing them. */
static ULONG
AhciStartCommand(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PATA_REQUEST Req
)
{
    PAHCI_PORT_INFO pi;
    PUCHAR portBase;
    PAHCI_COMMAND_HEADER cmdHeader;
    PUCHAR table;
    PUCHAR acmd;
    PAHCI_PRDT_ENTRY prdt;
    ULONG prdtEntries;
    ULONG i;

    pi = AHCI_PORTINFO(HwInit, Port);
    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, Port);

    if (pi->Outstanding != 0 || pi->AsyncSrb != NULL) {
        AhciDrainPort(HwInit, Port);
    }

    cmdHeader = &pi->CommandList[0];
    table = AHCI_SLOT_TABLE(pi, 0);
    acmd = table + AHCI_CMDTBL_ACMD_OFFSET;
    prdt = (PAHCI_PRDT_ENTRY)(table + AHCI_CMDTBL_PRDT_OFFSET);

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
                                ((ULONG)(Req->Pmp & 0x0F) << AHCI_CMDHDR_PMP_SHIFT) |
                                (Req->IsAtapi ? AHCI_CMDHDR_ATAPI : 0) |
                                ((Req->IsWrite && Req->DataBufferLen > 0) ? AHCI_CMDHDR_WRITE : 0));
    cmdHeader->PrdtLength = (USHORT)prdtEntries;
    cmdHeader->PrdByteCount = 0;
    cmdHeader->CommandTableBase = pi->CommandTablesPhysical;
    cmdHeader->CommandTableBaseUpper = pi->CommandTablesPhysicalUpper;

    AhciFillFis((PFIS_REG_H2D)table, Req);
    if (Req->IsAtapi) {
        ZeroMemoryBytes(acmd, 16);
        for (i = 0; i < Req->CdbLength && i < 16; i++) acmd[i] = Req->Cdb[i];
    }

    /* Result registers: mark the FIS areas stale before the command */
    pi->ReceivedFis[AHCI_RFIS_PIO_SETUP] = 0;
    pi->ReceivedFis[AHCI_RFIS_D2H] = 0;

    /* Go */
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << Port));

    AHCI_WRITE_REG(portBase, AHCI_PORT_CI, 1);
    return AHCI_CMD_OK;
}

/* The end of a command on slot 0: status cleared, result registers, the
   device's last task file; the port is restarted after an error */
static ULONG
AhciFinishCommand(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port,
    IN UCHAR Pmp,
    IN UCHAR Command,
    IN ULONG Result,
    IN ULONG PortIs,
    IN ULONG Ci,
    IN ULONG Tfd,
    IN BOOLEAN TimedOut
)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    PUCHAR portBase = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    ULONG i;

    if (PortIs != 0) AHCI_WRITE_REG(portBase, AHCI_PORT_IS, PortIs);
    if (PortIs & AHCI_PORT_IS_FATAL) AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << Port));

    /* PIO data-in ends with a PIO Setup FIS and no D2H FIS */
    AhciReadResult(HwInit, Port,
                   (BOOLEAN)(pi->ReceivedFis[AHCI_RFIS_D2H] != 0x34 &&
                             pi->ReceivedFis[AHCI_RFIS_PIO_SETUP] == 0x5F));
    for (i = 0; i < pi->DeviceCount; i++) {
        if (AHCI_DEVICE_OF(HwInit, Port, i)->Pmp == Pmp) AHCI_DEVICE_OF(HwInit, Port, i)->LastTfd = Tfd;
    }

    if (Result != AHCI_CMD_OK) {
        AHCI_DBG((AHCI_PFX "Port %lu.%lu: cmd %02lX %s, TFD=%08lX IS=%08lX CI=%08lX\n",
                  Port, (ULONG)Pmp, (ULONG)Command, TimedOut ? "timed out" : "failed", Tfd, PortIs, Ci));
        AhciRestartPort(HwInit, Port, FALSE);
    }
    return Result;
}

/* Issue one command on slot 0 and poll it to completion (Fast Polling) */
ULONG
AhciRunCommand(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PATA_REQUEST Req
)
{
    PUCHAR portBase = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    ULONG elapsed;
    ULONG timeoutUsec;
    ULONG ci, portIs, tfd;
    ULONG result;

    result = AhciStartCommand(HwInit, Port, Srb, Req);
    if (result != AHCI_CMD_OK) return result;

    if (Req->TimeoutSec != 0) {
        timeoutUsec = Req->TimeoutSec;
    } else if (Srb == NULL) {
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

        /* PxTFD may still hold ERR from an earlier command until this
           one's first FIS arrives: errors come from PxIS.TFES */
        if (portIs & AHCI_PORT_IS_FATAL) {
            result = (portIs & AHCI_PORT_IS_TFES) ? AHCI_CMD_DEVICE_ERROR : AHCI_CMD_HBA_ERROR;
            break;
        }
        if (!(ci & 1)) {
            if (tfd & TFD_STS_ERR) result = AHCI_CMD_DEVICE_ERROR;
            break;
        }
        if (elapsed >= timeoutUsec) {
            result = AHCI_CMD_HBA_ERROR;
            break;
        }
        AHCI_POLL_STEP(elapsed, 3);
    }

    return AhciFinishCommand(HwInit, Port, Req->Pmp, (UCHAR)(Req->IsAtapi ? Req->Cdb[0] : Req->Command),
                             result, portIs, ci, tfd, (BOOLEAN)(elapsed >= timeoutUsec));
}

/* A command for an SRB, completed by the interrupt where there is one:
   AHCI_CMD_PENDING when issued (AhciSatlAsyncDone finishes the SRB),
   otherwise the result of Fast Polling. The time limit is ScsiPort's: its
   timeout resets the bus, which fails the command. */
ULONG
AhciRunAsync(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PATA_REQUEST Req,
    IN UCHAR Kind,
    IN UCHAR Flags
)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    ULONG result;

    if (!HwInit->AsyncMode || HwInit->DumpMode || Srb == NULL || Req->UseBounce) {
        return AhciRunCommand(HwInit, Port, Srb, Req);
    }
    result = AhciStartCommand(HwInit, Port, Srb, Req);
    if (result != AHCI_CMD_OK) return result;
    pi->AsyncSrb = Srb;
    pi->AsyncKind = Kind;
    pi->AsyncFlags = Flags;
    pi->AsyncPmp = Req->Pmp;
    pi->AsyncAtapi = Req->IsAtapi;
    pi->AsyncCommand = (UCHAR)(Req->IsAtapi ? Req->Cdb[0] : Req->Command);
    AhciArmTimer(HwInit);
    ScsiPortNotification(NextRequest, HwInit, NULL);
    return AHCI_CMD_PENDING;
}

/* The command of the interrupt path on slot 0 ended (or failed): result,
   then the SRB's completion */
static VOID
AhciAsyncEnded(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG PortIs)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    PUCHAR portBase = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    ULONG ci = AHCI_READ_REG(portBase, AHCI_PORT_CI);
    ULONG tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);
    ULONG result = AHCI_CMD_OK;
    PSCSI_REQUEST_BLOCK srb = pi->AsyncSrb;

    pi->AsyncSrb = NULL;
    if (PortIs & AHCI_PORT_IS_FATAL) {
        result = (PortIs & AHCI_PORT_IS_TFES) ? AHCI_CMD_DEVICE_ERROR : AHCI_CMD_HBA_ERROR;
    } else if (tfd & TFD_STS_ERR) {
        result = AHCI_CMD_DEVICE_ERROR;
    }
    result = AhciFinishCommand(HwInit, Port, pi->AsyncPmp, pi->AsyncCommand, result, PortIs, ci, tfd, FALSE);
    AhciSatlAsyncDone(HwInit, Port, srb, result);
}

/* Non-data ATA command to one device */
ULONG
AhciRunSimple(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port,
    IN ULONG Lun,
    IN UCHAR Command,
    IN USHORT Features,
    IN ULONG TimeoutSec
)
{
    ATA_REQUEST req;

    ZeroMemoryBytes(&req, sizeof(req));
    req.Pmp = AHCI_DEVICE_OF(HwInit, Port, Lun)->Pmp;
    req.Command = Command;
    req.Features = Features;
    req.TimeoutSec = TimeoutSec;
    return AhciRunCommand(HwInit, Port, NULL, &req);
}

/* IDENTIFY (PACKET) DEVICE; the data stays in the port's bounce buffer
   for AhciParseIdentify */
BOOLEAN
AhciIdentifyDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Lun);
    PUSHORT id = (PUSHORT)pi->BounceBuffer;
    ATA_REQUEST req;
    ULONG result;

    ZeroMemoryBytes(pi->BounceBuffer, 512);
    ZeroMemoryBytes(&req, sizeof(req));
    req.Pmp = dev->Pmp;
    req.Command = dev->IsAtapi ? IDE_COMMAND_IDENTIFY_PACKET : IDE_COMMAND_IDENTIFY_DEVICE;
    req.UseBounce = TRUE;
    req.DataBufferLen = 512;
    /* ATAPI drives may still be spinning up after a reset */
    req.TimeoutSec = AHCI_POLL_TIMEOUT_INTERNAL_SEC;

    result = AhciRunCommand(HwInit, Port, NULL, &req);
    if (result == AHCI_CMD_OK && (id[IDW_MODEL] != 0 || id[IDW_GENERAL_CONFIG] != 0)) {
        dev->IdentifyValid = TRUE;
    } else {
        dev->IdentifyValid = FALSE;
        AHCI_DBG((AHCI_PFX "Port %lu.%lu: IDENTIFY failed (%lu), TFD=%08lX\n",
                  Port, (ULONG)dev->Pmp, result, dev->LastTfd));
    }
    return dev->IdentifyValid;
}

/* SET FEATURES "enable SATA feature" */
static ULONG
AhciEnableSataFeature(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun, IN UCHAR Feature)
{
    ATA_REQUEST req;

    ZeroMemoryBytes(&req, sizeof(req));
    req.Pmp = AHCI_DEVICE_OF(HwInit, Port, Lun)->Pmp;
    req.Command = IDE_COMMAND_SET_FEATURES;
    req.Features = IDE_FEATURE_ENABLE_SATA;
    req.SectorCount = Feature;
    return AhciRunCommand(HwInit, Port, NULL, &req);
}

/* Link power management for a device directly on the port, on unless
   ALPM=0 / DEVSLP=0: the HBA takes the idle link to Partial (ALPM=2:
   Slumber) where the device takes host-initiated requests, the device
   does it on its own too where it can (DIPM), and DevSleep follows after
   a second without commands where HBA and device have it. A command
   wakes the link again. */
static VOID
AhciApplyLinkPower(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Lun);
    PUCHAR pb = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    ULONG cap = HwInit->HbaCapabilities;
    ULONG cap2 = AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_CAP2);
    USHORT feat = dev->Id[AHCI_ID_SATA_FEATURES];
    ULONG cmd, ds, t;

    if (pi->PmAttached || !dev->IdentifyValid || HwInit->DumpMode) return;

    if (HwInit->LinkPm != 0 && (cap & AHCI_CAP_SALP) && (dev->Id[AHCI_ID_SATA_CAPS] & IDW76_HIPM)) {
        BOOLEAN slumber = (BOOLEAN)(HwInit->LinkPm == 2 && (cap & AHCI_CAP_SSC));
        if (slumber || (cap & AHCI_CAP_PSC)) {
            if (feat & IDW78_DIPM) AhciEnableSataFeature(HwInit, Port, Lun, SATA_FEATURE_DIPM);
            cmd = AHCI_READ_REG(pb, AHCI_PORT_CMD) | AHCI_PORT_CMD_ALPE;
            cmd = slumber ? (cmd | AHCI_PORT_CMD_ASP) : (cmd & ~AHCI_PORT_CMD_ASP);
            AHCI_WRITE_REG(pb, AHCI_PORT_CMD, cmd & ~AHCI_PORT_CMD_ICC_MASK);
            pi->LpmOn = TRUE;
            AHCI_DBG((AHCI_PFX "Port %lu: link power management (%s%s)\n", Port,
                      slumber ? "Slumber" : "Partial", (feat & IDW78_DIPM) ? ", device too" : ""));
        }
    }

    ds = AHCI_READ_REG(pb, AHCI_PORT_DEVSLP);
    if (HwInit->DevSleep && (cap2 & AHCI_CAP2_SDS) && (cap2 & AHCI_CAP2_SADM) &&
        (feat & IDW78_DEVSLP) && (ds & AHCI_DEVSLP_DSP) && !(ds & AHCI_DEVSLP_ADSE)) {
        if (AhciEnableSataFeature(HwInit, Port, Lun, SATA_FEATURE_DEVSLP) != AHCI_CMD_OK) return;
        /* Timings only with the engine stopped (AHCI 1.3.1, 3.3.17):
           exit timeout 20 ms, minimum assertion 10 ms, idle 1000 ms */
        AhciStopPortEngines(HwInit, pb);
        AHCI_WRITE_REG(pb, AHCI_PORT_DEVSLP, (ds & AHCI_DEVSLP_DSP) |
                       (20UL << AHCI_DEVSLP_DETO_SHIFT) | (10UL << AHCI_DEVSLP_MDAT_SHIFT) |
                       (1000UL << AHCI_DEVSLP_DITO_SHIFT) | AHCI_DEVSLP_ADSE);
        AHCI_WRITE_REG(pb, AHCI_PORT_CMD, AHCI_READ_REG(pb, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
        t = 500;
        while ((AHCI_READ_REG(pb, AHCI_PORT_TFD) & (TFD_STS_BSY | TFD_STS_DRQ)) && --t) ScsiPortStallExecution(1000);
        AHCI_WRITE_REG(pb, AHCI_PORT_CMD, AHCI_READ_REG(pb, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
        pi->DevSlpOn = TRUE;
        AHCI_DBG((AHCI_PFX "Port %lu: DevSleep after 1 s idle\n", Port));
    }
}

/* Before a shutdown or a power transition: link power management and
   DevSleep off, the link brought to Active. The next set-up (power up,
   reset, re-initialization) turns them on again. */
VOID
AhciLinkFullPower(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    PUCHAR pb = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    ULONG cmd, t;

    if (!pi->LpmOn && !pi->DevSlpOn) return;
    if (pi->DevSlpOn) {
        AhciDrainPort(HwInit, Port);            /* the engines stop for the change */
        AhciStopPortEngines(HwInit, pb);
        AHCI_WRITE_REG(pb, AHCI_PORT_DEVSLP, AHCI_READ_REG(pb, AHCI_PORT_DEVSLP) & ~AHCI_DEVSLP_ADSE);
        AHCI_WRITE_REG(pb, AHCI_PORT_CMD, AHCI_READ_REG(pb, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
        AHCI_WRITE_REG(pb, AHCI_PORT_CMD, AHCI_READ_REG(pb, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
        pi->DevSlpOn = FALSE;
    }
    cmd = AHCI_READ_REG(pb, AHCI_PORT_CMD) & ~(AHCI_PORT_CMD_ALPE | AHCI_PORT_CMD_ASP | AHCI_PORT_CMD_ICC_MASK);
    AHCI_WRITE_REG(pb, AHCI_PORT_CMD, cmd);
    AHCI_WRITE_REG(pb, AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_ICC_ACTIVE);
    /* ICC reads 0 again once the HBA took it; Slumber to Active within 10 ms */
    for (t = 0; t < 100 && (AHCI_READ_REG(pb, AHCI_PORT_CMD) & AHCI_PORT_CMD_ICC_MASK); t++) {
        ScsiPortStallExecution(100);
    }
    pi->LpmOn = FALSE;
    AHCI_DBG((AHCI_PFX "Port %lu: link at full power\n", Port));
}

/* Write cache / look-ahead as set through MODE SELECT; link power */
VOID
AhciApplyDeviceSettings(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Lun);
    UCHAR wc = pi->WantWriteCache[dev->Pmp & 0x0F];
    UCHAR la = pi->WantLookAhead[dev->Pmp & 0x0F];

    if (!dev->Present) return;
    if (HwInit->LinkPm != 0 || HwInit->DevSleep) AhciApplyLinkPower(HwInit, Port, Lun);
    if (dev->IsAtapi) return;

    /* SPINDOWN=n: the drive's own standby timer (IDLE with a count), so it
       spins down after n idle minutes on any Windows version */
    if (HwInit->SpinDown != 0 && dev->IdentifyValid && dev->RotationRate != 1) {
        ATA_REQUEST req;
        ZeroMemoryBytes(&req, sizeof(req));
        req.Pmp = dev->Pmp;
        req.Command = IDE_COMMAND_IDLE;
        req.SectorCount = HwInit->SpinDown;
        AhciRunCommand(HwInit, Port, NULL, &req);
    }
    if (wc != 0) {
        AhciRunSimple(HwInit, Port, Lun, IDE_COMMAND_SET_FEATURES,
                      (USHORT)((wc == 1) ? IDE_FEATURE_ENABLE_WRITE_CACHE : IDE_FEATURE_DISABLE_WRITE_CACHE), 0);
    }
    if (la != 0) {
        AhciRunSimple(HwInit, Port, Lun, IDE_COMMAND_SET_FEATURES,
                      (USHORT)((la == 1) ? IDE_FEATURE_ENABLE_LOOKAHEAD : IDE_FEATURE_DISABLE_LOOKAHEAD), 0);
    }
}

/* ------------------------------------------------------------------ */
/* Software reset and port multiplier registers                        */
/* ------------------------------------------------------------------ */

static BOOLEAN
AhciWaitSlot0(IN PUCHAR PortBase, IN ULONG TimeoutMs, IN BOOLEAN WaitNotBusy)
{
    ULONG t = TimeoutMs * 100;

    while (t--) {
        if (!(AHCI_READ_REG(PortBase, AHCI_PORT_CI) & 1) &&
            (!WaitNotBusy || !(AHCI_READ_REG(PortBase, AHCI_PORT_TFD) & (TFD_STS_BSY | TFD_STS_DRQ)))) {
            return TRUE;
        }
        if (AHCI_READ_REG(PortBase, AHCI_PORT_IS) & AHCI_PORT_IS_FATAL) return FALSE;
        ScsiPortStallExecution(10);
    }
    return FALSE;
}

/* SRST to one PM port (15 = the PM itself), AHCI 10.4.1. The signature
   comes back in the D2H FIS that ends the reset. */
BOOLEAN
AhciSoftReset(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN UCHAR Pmp, OUT PULONG Signature)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    PUCHAR portBase = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    PAHCI_COMMAND_HEADER cmdHeader = &pi->CommandList[0];
    PFIS_REG_H2D fis = (PFIS_REG_H2D)AHCI_SLOT_TABLE(pi, 0);
    PUCHAR d2h = pi->ReceivedFis + AHCI_RFIS_D2H;
    ULONG pmpBits = (ULONG)(Pmp & 0x0F) << AHCI_CMDHDR_PMP_SHIFT;

    *Signature = 0xFFFFFFFF;
    if (pi->Outstanding != 0) AhciDrainPort(HwInit, Port);

    /* A device still busy blocks the H2D FIS: command list override,
       which may only be set while the engine is stopped */
    if ((AHCI_READ_REG(portBase, AHCI_PORT_TFD) & (TFD_STS_BSY | TFD_STS_DRQ)) &&
        (HwInit->HbaCapabilities & AHCI_CAP_SCLO)) {
        ULONG t = 50000;
        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) & ~AHCI_PORT_CMD_ST);
        while ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) && --t) {
            ScsiPortStallExecution(10);
        }
        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_CLO);
        t = 5000;
        while ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CLO) && --t) {
            ScsiPortStallExecution(10);
        }
        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);

    /* First FIS: SRST set, no D2H FIS follows; the HBA clears PxCI */
    ZeroMemoryBytes(fis, sizeof(FIS_REG_H2D));
    fis->FisType = FIS_TYPE_REG_H2D;
    fis->PmPortControl = (UCHAR)(Pmp & 0x0F);
    fis->Control = FIS_CONTROL_SRST;
    cmdHeader->Flags = (USHORT)(AHCI_CMDHDR_CFL_H2D | AHCI_CMDHDR_RESET | AHCI_CMDHDR_CLEAR_BUSY | pmpBits);
    cmdHeader->PrdtLength = 0;
    cmdHeader->PrdByteCount = 0;
    cmdHeader->CommandTableBase = pi->CommandTablesPhysical;
    cmdHeader->CommandTableBaseUpper = pi->CommandTablesPhysicalUpper;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CI, 1);
    if (!AhciWaitSlot0(portBase, 500, FALSE)) {
        AHCI_DBG((AHCI_PFX "Port %lu.%lu: SRST set did not complete\n", Port, (ULONG)Pmp));
        AhciRestartPort(HwInit, Port, FALSE);
        return FALSE;
    }
    ScsiPortStallExecution(20);

    /* Second FIS: SRST clear; the device answers with its signature */
    d2h[0] = 0;
    ZeroMemoryBytes(fis, sizeof(FIS_REG_H2D));
    fis->FisType = FIS_TYPE_REG_H2D;
    fis->PmPortControl = (UCHAR)(Pmp & 0x0F);
    cmdHeader->Flags = (USHORT)(AHCI_CMDHDR_CFL_H2D | pmpBits);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CI, 1);
    if (!AhciWaitSlot0(portBase, 10000, TRUE) || d2h[0] != 0x34) {
        AHCI_DBG((AHCI_PFX "Port %lu.%lu: SRST clear: no signature (TFD=%08lX)\n",
                  Port, (ULONG)Pmp, AHCI_READ_REG(portBase, AHCI_PORT_TFD)));
        AhciRestartPort(HwInit, Port, FALSE);
        return FALSE;
    }

    AhciReadResult(HwInit, Port, FALSE);
    *Signature = (ULONG)pi->Result.CountLow | ((ULONG)pi->Result.Lba0 << 8) |
                 ((ULONG)pi->Result.Lba1 << 16) | ((ULONG)pi->Result.Lba2 << 24);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << Port));
    return TRUE;
}

/* READ PORT MULTIPLIER: register Reg of PM port PmPort (15 = GSCR).
   The value comes back in Count, LBA low, mid and high. */
BOOLEAN
AhciPmRead(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN UCHAR PmPort, IN ULONG Reg, OUT PULONG Value)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    ATA_REQUEST req;

    ZeroMemoryBytes(&req, sizeof(req));
    req.Pmp = AHCI_PMP_CONTROL;
    req.Command = IDE_COMMAND_READ_PM;
    req.Features = (USHORT)Reg;
    req.Is48Bit = TRUE;
    req.RawDevice = TRUE;
    req.Device = (UCHAR)(PmPort & 0x0F);
    req.TimeoutSec = 1;

    *Value = 0;
    if (AhciRunCommand(HwInit, Port, NULL, &req) != AHCI_CMD_OK) return FALSE;
    *Value = (ULONG)pi->Result.CountLow | ((ULONG)pi->Result.Lba0 << 8) |
             ((ULONG)pi->Result.Lba1 << 16) | ((ULONG)pi->Result.Lba2 << 24);
    return TRUE;
}

BOOLEAN
AhciPmWrite(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN UCHAR PmPort, IN ULONG Reg, IN ULONG Value)
{
    ATA_REQUEST req;

    ZeroMemoryBytes(&req, sizeof(req));
    req.Pmp = AHCI_PMP_CONTROL;
    req.Command = IDE_COMMAND_WRITE_PM;
    req.Features = (USHORT)Reg;
    req.Is48Bit = TRUE;
    req.RawDevice = TRUE;
    req.Device = (UCHAR)(PmPort & 0x0F);
    req.SectorCount = Value & 0xFF;
    req.Lba.Low = Value >> 8;
    req.TimeoutSec = 1;
    return (BOOLEAN)(AhciRunCommand(HwInit, Port, NULL, &req) == AHCI_CMD_OK);
}

/* ------------------------------------------------------------------ */
/* NCQ                                                                 */
/* ------------------------------------------------------------------ */

static ULONG
AhciSlotMask(IN ULONG Depth)
{
    return (Depth >= 32) ? 0xFFFFFFFF : ((1UL << Depth) - 1);
}

static VOID
AhciCompleteSlot(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Slot, IN UCHAR SrbStatus)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    PSCSI_REQUEST_BLOCK srb = pi->SlotSrb[Slot];

    pi->SlotSrb[Slot] = NULL;
    pi->Outstanding &= ~(1UL << Slot);
    if (srb == NULL) return;

    srb->SrbStatus = SrbStatus;
    if (SrbStatus == SRB_STATUS_SUCCESS) srb->ScsiStatus = SCSISTAT_GOOD;
    ScsiPortNotification(RequestComplete, HwInit, srb);

    if (pi->LuBlocked) {
        pi->LuBlocked = FALSE;
        ScsiPortNotification(NextLuRequest, HwInit, srb->PathId, srb->TargetId, srb->Lun);
    }
}

VOID
AhciFailOutstanding(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN UCHAR SrbStatus)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    ULONG slot;

    for (slot = 0; slot < AHCI_MAX_SLOTS; slot++) {
        if (pi->Outstanding & (1UL << slot)) {
            AhciCompleteSlot(HwInit, Port, slot, SrbStatus);
        }
    }
    pi->Outstanding = 0;
    if (pi->AsyncSrb != NULL) {
        PSCSI_REQUEST_BLOCK srb = pi->AsyncSrb;
        pi->AsyncSrb = NULL;
        srb->SrbStatus = SrbStatus;
        ScsiPortNotification(RequestComplete, HwInit, srb);
        ScsiPortNotification(NextRequest, HwInit, NULL);
    }
}

/* Queue READ/WRITE FPDMA QUEUED on a free tag. The slot number is the
   tag. Waits for a slot when all are busy. Returns SRB_STATUS_PENDING
   when queued, otherwise the status to complete the SRB with. */
UCHAR
AhciIssueNcq(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN AHCI_LBA Lba,
    IN ULONG Count,
    IN BOOLEAN IsWrite,
    IN BOOLEAN Fua
)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Srb->Lun);
    PUCHAR portBase = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    ULONG depth, freeMask, slot, prdtEntries, waited;
    PUCHAR table;
    PFIS_REG_H2D fis;
    PAHCI_COMMAND_HEADER cmdHeader;

    if (pi->AsyncSrb != NULL) AhciDrainPort(HwInit, Port);
    depth = dev->NcqDepth;
    if (depth > HwInit->Slots) depth = HwInit->Slots;

    /* Normally ScsiPort holds requests back while the slots are full */
    waited = 0;
    while ((freeMask = (~pi->Outstanding) & AhciSlotMask(depth)) == 0) {
        AhciServicePort(HwInit, Port);
        if (waited++ > AHCI_POLL_TIMEOUT_MAX_SEC * 100000UL) {
            AHCI_DBG((AHCI_PFX "Port %lu: no NCQ slot freed\n", Port));
            AhciFailOutstanding(HwInit, Port, SRB_STATUS_BUS_RESET);
            AhciRestartPort(HwInit, Port, TRUE);
            return SRB_STATUS_BUS_RESET;
        }
        ScsiPortStallExecution(10);
    }

    /* Round robin over the tags */
    slot = pi->NextSlot % depth;
    while (!(freeMask & (1UL << slot))) slot = (slot + 1) % depth;
    pi->NextSlot = slot + 1;

    table = AHCI_SLOT_TABLE(pi, slot);
    fis = (PFIS_REG_H2D)table;
    cmdHeader = &pi->CommandList[slot];

    if (!AhciBuildPrdt(HwInit, Srb, (PAHCI_PRDT_ENTRY)(table + AHCI_CMDTBL_PRDT_OFFSET),
                       Srb->DataBuffer, Count * (dev->SectorSize >> dev->LbaShift), &prdtEntries)) {
        return SRB_STATUS_INVALID_REQUEST;
    }

    ZeroMemoryBytes(fis, sizeof(FIS_REG_H2D));
    fis->FisType = FIS_TYPE_REG_H2D;
    fis->PmPortControl = FIS_H2D_COMMAND;
    fis->Command = IsWrite ? IDE_COMMAND_WRITE_FPDMA : IDE_COMMAND_READ_FPDMA;
    fis->FeaturesLow = (UCHAR)Count;                /* 65536 = 0 */
    fis->FeaturesHigh = (UCHAR)(Count >> 8);
    fis->SectorCountLow = (UCHAR)(slot << 3);       /* tag */
    fis->Lba0 = (UCHAR)(Lba.Low);
    fis->Lba1 = (UCHAR)(Lba.Low >> 8);
    fis->Lba2 = (UCHAR)(Lba.Low >> 16);
    fis->Lba3 = (UCHAR)(Lba.Low >> 24);
    fis->Lba4 = (UCHAR)(Lba.High);
    fis->Lba5 = (UCHAR)(Lba.High >> 8);
    fis->Device = (UCHAR)(ATA_DEV_LBA | (Fua ? ATA_DEV_FUA : 0));

    cmdHeader->Flags = (USHORT)(AHCI_CMDHDR_CFL_H2D | (IsWrite ? AHCI_CMDHDR_WRITE : 0));
    cmdHeader->PrdtLength = (USHORT)prdtEntries;
    cmdHeader->PrdByteCount = 0;
    cmdHeader->CommandTableBase = pi->CommandTablesPhysical + slot * pi->TableSize;
    cmdHeader->CommandTableBaseUpper = pi->CommandTablesPhysicalUpper;

    pi->SlotSrb[slot] = Srb;
    pi->Outstanding |= (1UL << slot);

#ifdef AHCI_DBG_ENABLED
    {
        ULONG n = 0, i;
        for (i = 0; i < AHCI_MAX_SLOTS; i++) if (pi->Outstanding & (1UL << i)) n++;
        if (n > pi->MaxQueued) {
            pi->MaxQueued = (UCHAR)n;
            if ((n & (n - 1)) == 0) AHCI_DBG((AHCI_PFX "Port %lu: %lu NCQ commands queued\n", Port, n));
        }
    }
#endif

    AHCI_WRITE_REG(portBase, AHCI_PORT_SACT, (1UL << slot));
    AHCI_WRITE_REG(portBase, AHCI_PORT_CI, (1UL << slot));

    /* More for this LU while tags are free; otherwise only other LUs */
    if ((~pi->Outstanding) & AhciSlotMask(depth)) {
        ScsiPortNotification(NextLuRequest, HwInit, Srb->PathId, Srb->TargetId, Srb->Lun);
    } else {
        pi->LuBlocked = TRUE;
        ScsiPortNotification(NextRequest, HwInit, NULL);
    }
    AhciArmTimer(HwInit);
    return SRB_STATUS_PENDING;
}

/* Error during NCQ: the device stops queued processing until the NCQ
   error log is read (ATA8-ACS 4.20.4). The failed command gets the error,
   the others are given back to ScsiPort as busy for a retry. */
static VOID
AhciNcqRecover(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG PortIs)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    PUCHAR portBase = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, 0);
    ULONG saved = pi->Outstanding;
    PSCSI_REQUEST_BLOCK savedSrb[AHCI_MAX_SLOTS];
    ULONG tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);
    ULONG slot, failedTag = 0xFFFFFFFF;
    UCHAR status = 0, error = 0;
    ATA_REQUEST req;

    AHCI_DBG((AHCI_PFX "Port %lu: NCQ error, IS=%08lX TFD=%08lX SACT=%08lX outstanding=%08lX\n",
              Port, PortIs, tfd, AHCI_READ_REG(portBase, AHCI_PORT_SACT), saved));

    for (slot = 0; slot < AHCI_MAX_SLOTS; slot++) savedSrb[slot] = pi->SlotSrb[slot];

    /* Stopping the engine clears PxCI and PxSACT */
    pi->Outstanding = 0;
    AhciRestartPort(HwInit, Port, (BOOLEAN)(!(PortIs & AHCI_PORT_IS_TFES)));
    pi->Outstanding = saved;

    if ((PortIs & AHCI_PORT_IS_TFES) && !pi->NeedsSetup) {
        ZeroMemoryBytes(&req, sizeof(req));
        req.Command = IDE_COMMAND_READ_LOG_EXT;
        req.Is48Bit = TRUE;
        req.Lba.Low = ATA_LOG_NCQ_ERROR;
        req.SectorCount = 1;
        req.UseBounce = TRUE;
        req.DataBufferLen = 512;
        saved = pi->Outstanding;
        pi->Outstanding = 0;
        if (AhciRunCommand(HwInit, Port, NULL, &req) == AHCI_CMD_OK &&
            !(pi->BounceBuffer[0] & 0x80)) {
            failedTag = pi->BounceBuffer[0] & 0x1F;
            status = pi->BounceBuffer[2];
            error = pi->BounceBuffer[3];
        }
        pi->Outstanding = saved;
    }

    /* No NCQ error log: with one command outstanding it is the one that
       failed. Otherwise all are retried, and NCQ goes off for a device
       that keeps failing this way. */
    if (failedTag == 0xFFFFFFFF && (PortIs & AHCI_PORT_IS_TFES) &&
        pi->Outstanding != 0 && (pi->Outstanding & (pi->Outstanding - 1)) == 0) {
        for (failedTag = 0; !(pi->Outstanding & (1UL << failedTag)); failedTag++) ;
        status = (UCHAR)tfd;
        error = (UCHAR)(tfd >> TFD_ERR_SHIFT);
    }
    if (failedTag == 0xFFFFFFFF && ++dev->NcqErrors >= AHCI_NCQ_ERROR_LIMIT && dev->Ncq) {
        AHCI_DBG((AHCI_PFX "Port %lu: NCQ off after %lu errors\n", Port, (ULONG)dev->NcqErrors));
        dev->Ncq = FALSE;
    }

    for (slot = 0; slot < AHCI_MAX_SLOTS; slot++) {
        if (!(pi->Outstanding & (1UL << slot))) continue;
        if (slot == failedTag && savedSrb[slot] != NULL) {
            PSCSI_REQUEST_BLOCK srb = savedSrb[slot];
            dev->LastTfd = ((ULONG)error << TFD_ERR_SHIFT) | status | TFD_STS_ERR;
            pi->SlotSrb[slot] = NULL;
            pi->Outstanding &= ~(1UL << slot);
            AhciSatlCompleteAtaError(HwInit, Port, srb);
            ScsiPortNotification(RequestComplete, HwInit, srb);
        } else {
            AhciCompleteSlot(HwInit, Port, slot, SRB_STATUS_BUSY);
        }
    }
    pi->Outstanding = 0;
    if (pi->LuBlocked) {
        pi->LuBlocked = FALSE;
        ScsiPortNotification(NextLuRequest, HwInit, 0, Port, 0);
    }
}

/* Complete what the device finished; handle errors. Called from the
   interrupt, the watchdog timer and while waiting for slots. */
ULONG
AhciServicePort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    PUCHAR portBase = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    ULONG portIs, busy, done, slot, n;

    portIs = AHCI_READ_REG(portBase, AHCI_PORT_IS);

    /* A command that is not queued: done when slot 0 is free or on error */
    if (pi->AsyncSrb != NULL) {
        if (!(portIs & AHCI_PORT_IS_FATAL) && (AHCI_READ_REG(portBase, AHCI_PORT_CI) & 1)) {
            if (portIs != 0) AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs & ~AHCI_PORT_IS_FATAL);
            AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << Port));
            return 0;
        }
        AhciAsyncEnded(HwInit, Port, portIs);
        return 1;
    }

    if (portIs != 0) AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << Port));

    if (pi->Outstanding == 0) return 0;

    if (portIs & AHCI_PORT_IS_FATAL) {
        n = 0;
        for (slot = 0; slot < AHCI_MAX_SLOTS; slot++) {
            if (pi->Outstanding & (1UL << slot)) n++;
        }
        AhciNcqRecover(HwInit, Port, portIs);
        return n;
    }

    busy = AHCI_READ_REG(portBase, AHCI_PORT_SACT) | AHCI_READ_REG(portBase, AHCI_PORT_CI);
    done = pi->Outstanding & ~busy;
    n = 0;
    for (slot = 0; done != 0 && slot < AHCI_MAX_SLOTS; slot++) {
        if (done & (1UL << slot)) {
            done &= ~(1UL << slot);
            AhciCompleteSlot(HwInit, Port, slot, SRB_STATUS_SUCCESS);
            n++;
        }
    }
    return n;
}

/* Poll until no queued command is left on the port */
VOID
AhciDrainPort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port)
{
    PAHCI_PORT_INFO pi = AHCI_PORTINFO(HwInit, Port);
    ULONG elapsed = 0;

    while (pi->Outstanding != 0 || pi->AsyncSrb != NULL) {
        AhciServicePort(HwInit, Port);
        if (pi->Outstanding == 0 && pi->AsyncSrb == NULL) break;
        if (elapsed >= AHCI_POLL_TIMEOUT_MAX_SEC * 1000000UL) {
            AHCI_DBG((AHCI_PFX "Port %lu: queued commands did not finish (%08lX)\n", Port, pi->Outstanding));
            AhciFailOutstanding(HwInit, Port, SRB_STATUS_BUS_RESET);
            AhciRestartPort(HwInit, Port, TRUE);
            break;
        }
        AHCI_POLL_STEP(elapsed, 1);
    }
}
