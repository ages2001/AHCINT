# AHCINT — SATA AHCI Driver for Windows 95/98/Me and Windows NT 3.1 to XP x64

**AHCINT** is a SCSI miniport driver for AHCI SATA controllers. It lets Windows 95, 98 and Me, Windows NT 3.1, 3.50, 3.51 and 4.0, and Windows 2000, XP and Server 2003 (x86 and x64) install on, boot from and use SATA disks and optical drives behind an AHCI controller — hardware these systems were never written for, and that Microsoft only supported from Windows Vista on.

| | |
|---|---|
| **Version** | 1.6.0.0 |
| **Operating systems** | Windows 95 / 98 / Me · Windows NT 3.1 / 3.50 / 3.51 / 4.0 · Windows 2000 / XP / Server 2003 (x86) · Windows XP x64 / Server 2003 x64 |
| **Hardware** | AHCI SATA host controllers (PCI class code `01-06-01`), up to 32 per system, all 32 ports of each |
| **Devices** | SATA disks (48-bit LBA, 512-byte to 4 KB logical sectors, SSDs with TRIM), ATAPI CD/DVD drives and removable ATAPI disks, disks behind port multipliers |
| **Binaries** | `ahcint9x.mpd` (Windows 9x/Me) · `ahcint.sys` (NT 3.1–4.0) · `ahcint.sys` (2000/XP/2003, x86 and x64) |

## Highlights

- **Install and boot** on every target: F6 / OEM driver disks for text-mode setup from NT 3.1 to XP x64, Have Disk for installed systems, Device Manager on Windows 9x
- **NCQ** with up to 32 queued commands per port, completed by interrupt, with an automatic fallback to Fast Polling where interrupts do not arrive
- **TRIM** for SSDs (SCSI UNMAP as ATA DATA SET MANAGEMENT), plus `tools\trimnt.exe` to trim the free space of a volume on systems that never send TRIM
- **Port multipliers**, ATAPI optical drives, 48-bit LBA, 16-byte CDBs and logical sectors up to 4 KB on every target
- **Up to 32 ports per controller and 32 controllers**, and single ports (or devices behind a port multiplier) can be turned off with `OFF=`
- **SMART** through the `atapi.sys`-compatible "SCSIDISK" IOCTL, ATA PASS-THROUGH, and LOG SENSE health and temperature pages
- **Power management**: flush at shutdown (standby with `STANDBY=1`), hibernation and crash dumps on 2000/XP, a power-down IOCTL for NT 3.1–4.0 and 9x
- **One source tree** for all targets; the differences are a handful of `#ifdef`s

- **Ports tab in Device Manager** (2000/XP/x64): check boxes to turn single ports off, without editing the registry
## Quick start

| System | Files | How |
|---|---|---|
| Windows 95 / 98 / Me | `bin\9X` | [Installing on Windows 95 / 98 / Me](#windows-95--98--me) |
| Windows NT 3.1 – 4.0 | `bin\NT\floppy` | [Installing on Windows NT](#windows-nt-31--350--351--nt-40) |
| Windows 2000 / XP / 2003, installed | `bin\2KXP` (Have Disk) | [GUI-mode setup](#gui-mode-setup-windows-2000xpxp-x64server-2003) |
| Windows 2000 / XP / 2003, new installation | `bin\2KXP\floppy_i386` or `floppy_amd64` (F6) | [Text-mode (F6) setup](#text-mode-f6-setup--windows-2000xpxp-x64server-2003) |

## Contents

- [Overview](#overview) · [Features](#features) · [Architecture](#architecture) · [Feature matrix by target](#feature-matrix-by-target) · [Known Limitations](#known-limitations)
- [Building from source](#building-from-source) · [Installation](#installation) · [Configuration](#configuration) · [Debugging](#debugging) · [Technical Notes](#technical-notes)
- [Version history](#version-history) · [Related drivers](#related-drivers) · [License](#license)

## Overview

AHCINT is a SCSI miniport driver built on the classic `ScsiPort` framework (`SCSIPORT.PDR` on Windows 9x/Me, `scsiport.sys` on the NT family). It talks directly to AHCI HBA/port registers (command lists, PRDTs, FIS structures) and translates SCSI requests from the Windows storage stack into ATA/ATAPI commands, so SATA AHCI controllers can be used as boot and data devices on operating systems that predate AHCI entirely.

### Why?

Because plenty of real hardware only exposes its SATA ports in AHCI mode, and 9x/Me/NT/2000/XP have no idea what that is.

## Features

- **AHCI 1.0-class hardware support**
  - HBA generic registers (GHC, IS, PI, CAP, CAP2) and per-port registers (CLB, FB, IS, IE, CMD, TFD, SIG, SSTS, SCTL, SERR, CI)
  - Command List / Command Header / PRDT / FIS-based command submission
  - Port state machine handling: COMRESET via `SCTL.DET`, FRE/ST start-stop sequencing, Command List Override (CLO) fallback when a port won't idle
  - No link speed limit: every COMRESET leaves `PxSCTL.SPD` at 0 (fastest speed both ends support); a limit the firmware left in `PxSCTL.SPD`, or a link slower than the HBA's `CAP.ISS`, gets one COMRESET at port initialization to renegotiate
  - BIOS/OS Handoff Control (BOHC) so the driver takes ownership cleanly from the firmware

- **SCSI-to-ATA Translation Layer (SATL)**
  - INQUIRY, REQUEST SENSE, TEST UNIT READY, MEDIUM REMOVAL, READ CAPACITY(10), MODE SENSE(6)/(10), REPORT LUNS
  - READ/WRITE(6), (10), (12) and (16) on all targets; FUA (Force Unit Access) as WRITE DMA FUA EXT, or the NCQ FUA bit
  - SYNCHRONIZE CACHE(10)/(16) as FLUSH CACHE (EXT); VERIFY(10)/(12)/(16) as READ VERIFY SECTORS (EXT)
  - START STOP UNIT as STANDBY IMMEDIATE / IDLE IMMEDIATE (the write cache is flushed first)
  - MODE SENSE caching page (08h, WCE/DRA from IDENTIFY word 85) and control page (0Ah); MODE SELECT on the caching page turns the write cache and read look-ahead on and off (SET FEATURES 02h/82h, AAh/55h), kept across resets and power transitions
  - SAT ATA PASS-THROUGH(12)/(16) (non-data, PIO in/out, DMA; CK_COND and "return response information" with descriptor sense)
  - SMART through `IOCTL_SCSI_MINIPORT` ("SCSIDISK" signature, as `atapi.sys`), so `SMART_RCV_DRIVE_DATA` tools work; see [IOCTL interface](#ioctl-interface)
  - 64-bit SCSI on every target: READ CAPACITY(16) (SERVICE ACTION IN), READ/WRITE/VERIFY(16), SYNCHRONIZE CACHE(16), MODE SENSE(10) long LBA block descriptor (LLBAA); READ CAPACITY(10) returns `0xFFFFFFFF` for disks past 2 TB so the upper layer switches to READ CAPACITY(16)
  - ATA disks report INQUIRY VERSION `05h` (SPC-3 / SBC-2, as nvme9x does), advertising 16-byte CDB support; VPD pages `00h`, `80h` (serial number from IDENTIFY), `B0h` (block and unmap limits), `B1h` (rotation rate and form factor, IDENTIFY words 217/168) and `B2h` (logical block provisioning)
  - **UNMAP** as DATA SET MANAGEMENT / TRIM (IDENTIFY word 169) on SSDs: up to 64 block descriptors, split into TRIM ranges (64 per 512-byte block); READ CAPACITY(16) reports LBPME, and LBPRZ for deterministic zeroes after TRIM (word 69); see [TRIM](#trim)
  - **LOG SENSE** pages `00h`, `0Dh` (temperature from the SMART attributes) and `2Fh` (informational exceptions: ASC 5Dh when SMART RETURN STATUS reports a threshold exceeded)
  - **TRIM mode** for Windows versions that send no TRIM: with `tools\trimnt.exe` the free space of a volume is trimmed; see [TRIM](#trim)
  - 28-bit READ/WRITE DMA whenever the request fits; READ/WRITE DMA EXT only on devices whose IDENTIFY DEVICE reports the 48-bit Address feature set (word 83, bit 10)
  - ATAPI (PACKET command) pass-through for CD/DVD drives and removable ATAPI disks (the peripheral device type comes from IDENTIFY PACKET DEVICE), alongside plain ATA disk support; READ(6)/WRITE(6) go to the drive as READ(10)/WRITE(10)
  - Real ATAPI media status: TEST UNIT READY / MEDIUM REMOVAL are passed through to the drive, REQUEST SENSE returns the drive's own sense data, and MODE SENSE(6) is translated to ATAPI MODE SENSE(10) and back — so media changes and empty trays are reported correctly to the CD-ROM class driver
  - IDENTIFY DEVICE / IDENTIFY PACKET DEVICE based device enumeration, with cached string extraction for INQUIRY vendor/product fields
  - A port whose ATA device did not answer IDENTIFY DEVICE is reported as empty (`SRB_STATUS_NO_DEVICE`), never as a zero-length disk; an HBA without any device, or with none attached, just presents no targets, and the OS boots on
  - Logical sector sizes of 512, 1024, 2048 and 4096 bytes (IDENTIFY word 106 / words 117–118): READ CAPACITY(10)/(16) and MODE SENSE report the drive's own block length, READ CAPACITY(16) also the logical-per-physical exponent (512e drives); see [Logical sector size](#logical-sector-size)

- **Multi-target support from one source tree**
  - Windows 95 / 98 / Me (x86) — `SCSIPORT.PDR` miniport (`ahcint9x.mpd`)
  - Windows NT 3.1 / 3.50 / 3.51 / NT 4.0 (x86) — `ahcint.sys` (one binary for all four)
  - Windows 2000 / XP / Server 2003 (x86) and Windows XP x64 Edition / Server 2003 x64 (amd64) — `ahcint.sys`
  - Every target has the synchronous Fast Polling I/O path; NT 3.1 to XP x64 also queue commands with NCQ; the few real differences are `#ifdef`s (see [One source tree](#one-source-tree))

- **NCQ (Native Command Queuing)** — READ/WRITE FPDMA QUEUED with up to 32 tags per port, completed from `HwInterrupt` (SDB FIS), with a watchdog that falls back to Fast Polling if interrupts are not delivered, and NCQ error recovery (READ LOG EXT page 10h); see [NCQ](#ncq)

- **Port multipliers** — command-based switching; every device behind the multiplier is a LUN of its port; see [Port multipliers](#port-multipliers)

- **Power management** — flush at system shutdown (`SRB_FUNCTION_SHUTDOWN`, STANDBY IMMEDIATE too with `STANDBY=1`), flush and STANDBY IMMEDIATE on power transitions, `HwAdapterControl` (stop/restart) on 2000/XP, hibernation and crash dump support, automatic re-initialization when the HBA lost its state (sleep, BIOS, another driver), and a power-down/up IOCTL for an ACPI driver on NT 3.1–4.0 and 9x; see [Power management](#power-management)

## Architecture

```
          Windows 9x/Me                         Windows NT/2000/XP

        Application Layer                       Application Layer
               ↓                                       ↓
     IOS (I/O Supervisor) layers                SCSI Class Driver
               ↓                                       ↓
          SCSIPORT.PDR                            ScsiPort.sys
               ↓                                       ↓
  ahcint9x.mpd ← This driver               ahcint.sys  ← This driver
               ↓                                       ↓
               └─── AHCI Controller (PCI, 01-06-01) ───┘
                                   ↓
                   SATA disk / ATAPI optical drive
```

### Key Components

- **src\ahcimain.c** – Driver entry, PCI discovery (with bus-scan fallback), DMA arena, adapter/port initialization and re-initialization, port multiplier enumeration, `HwStartIo`, `HwInterrupt`, `HwResetBus`, `HwAdapterControl`, the NCQ interrupt watchdog; on 9X also `HwAdapterState`, the `_MapPhysToLinear` ABAR mapping and the VMM debug-print helper
- **src\ahcicmd.c** – The command engine: port start/stop/COMRESET, PRDT construction, the Fast Polling path, software reset, port multiplier register access, NCQ issue/completion/error recovery
- **src\ahcisatl.c** – SCSI command dispatch, SCSI↔ATA/ATAPI translation, 28/48-bit command selection, INQUIRY/READ CAPACITY/MODE SENSE/MODE SELECT, ATA PASS-THROUGH, sense handling, the IOCTL interface (SMART, power)
- **src\ahcint.h** – Target switches (`AHCI_W9X`, `AHCI_NT4`), AHCI register layout, command/FIS structures, device extension, shared constants
- **src\9X\** – `build.bat`, NMAKE `makefile` (includes the DDK's `MASTER.MK`) and `ahcint9x.lnk` for Windows 9x/Me
- **src\NT\** – `build.bat` for NT 3.1/3.50/3.51/4.0
- **src\2KXP\** – `sources` / `makefile` / `ahcint.rc` for the DDK `build` utility (2000/XP/2003, x86 and amd64)
- **tools\portpage.c**, **tools\mkpp.sh** – the Device Manager page `ahcipp.dll` for 2000/XP/x64 (see [Device Manager page](#device-manager-page)); the same source builds the page of IDENT, NVMENT and MMCSDNT
- **ahcint.inf** / **txtsetup.oem** / **oemsetup.inf** / **AHCINT9X.INF** – Installation files (in `bin\`) (see [Installation](#installation))

All targets compile the same four files in `src\`; the per-target folders only hold build files.

## Feature matrix by target

| | 95/98/Me (x86) | NT 3.1/3.50/3.51/4.0 (x86) | 2000/XP/2003 (x86) and XP x64/2003 x64 (amd64) |
|---|---|---|---|
| Build folder (sources are shared in `src\`) | `src\9X` | `src\NT` | `src\2KXP` |
| Target define | `AHCI_W9X` (implies `AHCI_NT4`) | `AHCI_NT4` | none |
| Driver binary | `ahcint9x.mpd` (`SCSIPORT.PDR` miniport) | `ahcint.sys` | `ahcint.sys` |
| I/O model | Synchronous Fast Polling — `HwStartIo` polls `PxCI`/`PxIS` every 10 µs, up to the SRB's `TimeOutValue` minus 1 s (see [Command timeout](#command-timeout)) | NCQ for disk reads/writes when the HBA has `CAP.SNCQ`, the disk supports NCQ and an interrupt is connected; Fast Polling for everything else (see [NCQ](#ncq)) | Same as NT |
| Interrupt usage | None — `PxIE` = 0 and `GHC.IE` off; `HwInterrupt` only acknowledges stray status | With NCQ: `PxIE` (SDB, D2H, errors) and `GHC.IE`; NT 3.1 connects one only with `NCQ=1` | With NCQ: as NT (PnP-assigned interrupt) |
| NCQ | No (`SCSIPORT.PDR` sends one request per device at a time) | Yes; NT 3.1 with `NCQ=1` | Yes; not in the crash dump / hibernation stack |
| Port multipliers | Yes (command-based switching) | Yes | Yes |
| Power management | Flush on shutdown; state check after `HwAdapterState` restore; power IOCTL | Flush on shutdown; power IOCTL; state check on every request | `HwAdapterControl` stop/restart, hibernation, crash dump; power IOCTL |
| SMART / ATA PASS-THROUGH | Yes | Yes | Yes |
| Max AHCI ports | 8 (build-time, see [Build-time limits](#build-time-limits)) | 8 (build-time) | 8 (build-time) |
| Max AHCI controllers | 8 (build-time, see [Build-time limits](#build-time-limits)) | 8 (build-time) | 8 (build-time) |
| PCI detection | Class code `01-06-01` plus known AMD/Intel device IDs; the bus:slot `SCSIPORT.PDR` hands in first, then a PCI bus-scan fallback | Same, bus:slot from ScsiPort first, then bus-scan fallback; NT 3.1 (no PCI support in its HAL) reads config space through ports `CF8h`/`CFCh` | Same, PnP-assigned bus:slot first, then bus-scan fallback |
| ABAR (MMIO) mapping | VMM `_MapPhysToLinear`, non-cached (KB Q169584 workaround) | `ScsiPortGetDeviceBase` | `ScsiPortGetDeviceBase` |
| ATA read/write commands | READ/WRITE DMA (28-bit) when the request ends below LBA 2^28 and moves ≤ 256 sectors; READ/WRITE DMA EXT only if IDENTIFY reports 48-bit support | Same | Same |
| READ/WRITE(16), READ CAPACITY(16), INQUIRY VERSION 05h (SPC-3) | Yes | Yes | Yes |
| Addressable capacity | 2^48 sectors (ATA limit) via READ CAPACITY(16); what the OS class driver can use is up to the OS | Same | Same |
| REQUEST SENSE | Yes — ATA errors (e.g. LBA out of range, UNC) and the driver's own sense are reported here; ATAPI asks the drive | Same | Yes |
| Auto-sense on error (`SRB_STATUS_AUTOSENSE_VALID`) | No — class driver issues REQUEST SENSE | No — class driver issues REQUEST SENSE | Yes — ATA errors mapped from `PxTFD`, ATAPI sense fetched from the drive |
| ATAPI (PACKET) support | Yes | Yes | Yes |
| ATAPI MODE SENSE(6)→(10) translation | Yes | Yes | Yes |
| 64-bit addressing (S64A) | No — 32-bit DMA only | No — 32-bit DMA only | Yes — 64-bit DMA when `CAP.S64A` is set (ABAR itself is read as a 32-bit BAR) |
| Hot-plug / late device detection | No | No | No |
| PRDT entries per command | 40 (33 at most in use: 32 physical breaks) | 40 | 40 |
| DMA arena size | Sized at run time: 3.5 KB per implemented port (one command table) | Same with Fast Polling; with NCQ one 768-byte command table per tag, at most 160 KB in all (see [Memory Allocation](#memory-allocation)) | Same as NT |
| Max transfer size per I/O | ~128 KB | ~128 KB | ~128 KB |
| Debug output | VMM `_Debug_Printf_Service` (`DEBUG=1` builds), prefix `[ahcint9x]` | `DbgPrint`, prefix `[AHCINT]` | `DbgPrint`, prefix `[AHCINT]` |

## Known Limitations

- No MSI/MSI-X; NCQ uses the legacy INTx line. Windows 9x and NT 3.1 (without `NCQ=1`) use Fast Polling only
- Everything that is not an NCQ disk read/write (ATAPI, IDENTIFY, FLUSH, devices behind a port multiplier, the crash dump stack) is Fast Polling: a slow command keeps the CPU that issued it busy polling for as long as it takes, up to the SRB timeout (see [Command timeout](#command-timeout))
- ~128 KB maximum transfer size per request (32 physical breaks), well short of AHCI's theoretical per-PRDT 4 MB limit
- No hot-plug support on any target; devices are detected at `HwInitialize` and again after a power transition or reset
- No FIS-based switching for port multipliers and no NCQ behind a port multiplier; the multiplier's own hot-plug notification (`PxIS.PCS`/SDB "N" bit) is not used
- No link power management (partial/slumber, `PxCMD.ALPE`) and no DevSleep; disks are spun down with STANDBY IMMEDIATE only on power-down, START STOP UNIT and (with `STANDBY=1`) shutdown
- SMART through the "SCSIDISK" IOCTL reaches only the first device of a port (LUN 0); devices behind a port multiplier are reached with ATA PASS-THROUGH
- Disks of 2 TB and more: Windows 9x, NT 3.1–4.0, 2000 and XP x86 address at most the first 2 TB (their class drivers know only READ CAPACITY(10) and 32-bit LBAs); XP x64 and Server 2003 SP1 and later see the whole disk. See [Disks of 2 TB and more](#disks-of-2-tb-and-more)
- Crash dump and hibernation (2000/XP/x64): the dump instance handles ports 0–7 only (see [Build-time limits](#build-time-limits))
- Logical sectors larger than 4096 bytes are not taken (the port is left unusable); CHS-only devices (no LBA) are not addressable (SATA requires LBA)
- A disk with logical sectors larger than 512 bytes works as a data disk only: BIOS INT 13h and the boot loaders read 512-byte sectors, so the system cannot boot from it
- 9X and NT builds do not fill auto-sense; the OS-era class drivers issue their own REQUEST SENSE
- Windows 95 RTM: see the [SCSIPORT.PDR note](#windows-95--98--me) below

## Building from source

### Requirements

- **Windows 95 / 98 / Me (x86):**
  - A Windows 9x build machine (the build script is written for the Windows 9x `COMMAND.COM` batch interpreter)
  - Windows 95 DDK — expected at `C:\DDK` (provides `MASTER.MK`, `BLOCK\INC`, `INC32`, `BLOCK\LIB\scsiport.lib`)
  - Win32 SDK for Windows NT 4.0 / Windows 95 — expected at `C:\MSTOOLS`
  - Microsoft Macro Assembler (MASM) 6.11 — expected at `C:\MASM611` (`BIN\ML.EXE`, `BIN\NMAKE.EXE`)
  - Microsoft Visual C++ 2.0 — expected at `C:\MSVC20` (`BIN\CL.EXE`, `BIN\LINK.EXE`, `BIN\NMAKE.EXE`)
- **Windows NT 3.1 / 3.50 / 3.51 / NT 4.0 (x86):** Windows NT 4.0 DDK + Visual C++ 4.0 (MSVC 4.0)
- **Windows 2000 / XP (x86):** Windows Server 2003 SP1 DDK ("WDK 6001", build 3790.1830)
- **Windows XP x64 / Server 2003 x64 (amd64):** A DDK/WDK with an amd64 ("WNET") cross-compiler — Windows Server 2003 SP1 DDK ("WDK 6001", build 3790.1830), Windows Server 2003 R2 DDK, or the Windows 7 WDK (WinDDK 7600.16385.1)

### Build Instructions

#### For Windows 95 / 98 / Me (x86)

`build.bat` expects a copy of the repository's `src` folder in `C:\AHCINT` (shared sources in `C:\AHCINT`, build files in `C:\AHCINT\9X`), and `ahcint9x.lnk` links against `C:\DDK\BLOCK\LIB\scsiport.lib`. If your tools live elsewhere, edit the `SET` lines at the top of `build.bat` (`MASM_ROOT`, `C16_ROOT`, `C32_ROOT`, `SDKROOT`, `DDKROOT`, `AHCI_ROOT`) and the library path in `ahcint9x.lnk`.

```bat
REM Copy the src folder (shared sources + 9X build files) to C:\AHCINT
mkdir C:\AHCINT
mkdir C:\AHCINT\9X
copy <path-to-AHCINT>\src\*.* C:\AHCINT
copy <path-to-AHCINT>\src\9X\*.* C:\AHCINT\9X

C:
cd \AHCINT\9X

REM Optional: debug build (debug output through VMM, see Debugging)
REM set DEBUG=1

REM Build
build.bat
```

`build.bat` sets `MASTER_MAKE=1` and the tool roots, puts MASM 6.11 and Visual C++ 2.0 on `PATH`, creates `TMP`/`TEMP` (default `C:\WINDOWS\TEMP`) if they are missing, warns if any expected tool is not found, and then runs `nmake` in `C:\AHCINT\9X`. The makefile pulls in the DDK's `MASTER.MK` (`BUILD_BITS=32`, `BUILD_TYPE=block`), compiles `..\ahcimain.c`, `..\ahcicmd.c` and `..\ahcisatl.c` with `-DAHCI_W9X`, and links them with `ahcint9x.lnk`.

Output: `C:\AHCINT\9X\ahcint9x.mpd` (plus `ahcint9x.map`). Run `nmake clean` to remove build output.

#### For Windows NT 3.1 / 3.50 / 3.51 / NT 4.0 (x86)

```bat
cd <path-to-AHCINT>\src\NT
build.bat
```

`build.bat` sets `MSVCDIR` (default `C:\MSDEV`) and `DDKDIR` (default `C:\NT4DDK`) — edit them if your tools live elsewhere — then compiles `..\ahcimain.c`, `..\ahcicmd.c` and `..\ahcisatl.c` with `-DAHCI_NT4` and links against `scsiport.lib` and `ntoskrnl.lib`.

Output: `src\NT\ahcint.sys`, for Windows NT 3.1 and later (`-subsystem:native,3.50`; the NT 3.1 loader does not check the subsystem version). The NT build imports only functions NT 3.1's `SCSIPORT.SYS` exports, so it must not call `ScsiPortSetBusDataByOffset`.

#### For Windows 2000 / XP (x86)

```bat
REM Set up build environment
Start Menu → Windows Server 2003 DDK → Build Environments → Windows 2000/XP Free Build Environment
REM (runs setenv.bat, configuring PATH/INCLUDE/LIB/DDKBUILDENV for the target)

REM Navigate to driver directory
cd <path-to-AHCINT>\src\2KXP

REM Build
build -cZ
```

`sources` compiles the shared `..\ahcimain.c`, `..\ahcicmd.c` and `..\ahcisatl.c` (no target define). `-c` forces a clean rebuild and `-Z` prints full build output to the console. Output: `obj\i386\ahcint.sys` (checked) or `obji386\ahcint.sys` (free), depending on the environment chosen. Use the **checked** environment for a debug build (asserts enabled, unoptimized), **free** for release.

#### For Windows XP x64 / Server 2003 x64 (amd64)

The x64 driver is built from the same `src\2KXP` folder as the x86 driver; only the build environment differs.

```bat
REM Set up build environment
Start Menu → Windows Server 2003 (WNET) DDK/WDK → Build Environments → x64 Free Build Environment
REM (runs setenv.bat ... WNET AMD64, configuring the amd64 cross-tools and libraries)

REM Navigate to driver directory
cd <path-to-AHCINT>\src\2KXP

REM Build
build -cZ
```

Output: `ahcint.sys` under the amd64 output directory (`objfre_wnet_amd64\amd64\` or equivalent, depending on the exact DDK/WDK version). `ahcint.rc` supplies version-resource information for this target.

> The 2000/XP and x64 builds should first be verified in the **checked** build environment before being rebuilt and shipped from the **free** build environment.

#### Device Manager page (2000/XP/x64)

`tools\mkpp.sh` builds `ahcipp.dll` (and the pages of the sibling drivers) from `tools\portpage.c` with MinGW-w64, x86 into `pp\i386` and x64 into `pp\amd64`. The DLL uses no C runtime: it imports only `kernel32`, `user32`, `advapi32`, `setupapi` and `comctl32` functions that Windows 2000 has.

## Installation

### Installation Media

Prebuilt binaries and install files live under `bin\`:

```
bin\
├── 9X\
│   ├── AHCINT9X.INF          (Windows 95/98/Me install file)
│   └── AHCINT9X.MPD          (SCSIPORT.PDR miniport)
├── NT\
│   ├── oemsetup.inf
│   ├── ahcint.sys
│   └── floppy\               (NT 3.50/3.51/4.0 driver disk)
│       ├── disk1             (tag file)
│       ├── oemsetup.inf
│       ├── txtsetup.oem
│       └── ahcint.sys
└── 2KXP\
    ├── ahcint.inf            (one INF for x86 and x64)
    ├── ahcint.sys            (x86 binary)
    ├── i386\
    │   └── ahcint.sys        (x86 binary)
    ├── amd64\
    │   └── ahcint.sys        (x64 binary)
    ├── floppy_i386\          (F6 disk, 2000/XP/2003 x86)
    │   ├── disk1             (tag file)
    │   ├── txtsetup.oem
    │   ├── ahcint.inf        (flat: no i386\ subdirectory)
    │   └── ahcint.sys
    └── floppy_amd64\         (F6 disk, XP x64/2003 x64)
        ├── disk1             (tag file)
        ├── txtsetup.oem
        ├── ahcint.inf        (flat: no amd64\ subdirectory)
        └── ahcint.sys
```

The F6 disks carry a flat copy of `ahcint.inf`: text-mode setup copies the driver and the INF into `%SystemRoot%\OemDir` without subdirectories, and GUI-mode setup installs the controller again from there. With the per-platform subdirectories of the main INF it would ask for `OemDir\amd64\ahcint.sys`.

All three INF/OEM files match the controller by PCI class code (`PCI\CC_010601`, Mass Storage - SATA - AHCI).

Every `bin\2KXP` folder also holds `ahcipp.dll`, the [Device Manager page](#device-manager-page): the x86 build in `2KXP\`, `i386\` and `floppy_i386\`, the x64 build in `amd64\` and `floppy_amd64\`. `ahcint.inf` copies it into `System32` and registers it (`EnumPropPages32`); `txtsetup.oem` lists it as a `dll` so that it is on the F6 disk's way into `OemDir` as well. Without it the driver works as before, only the tab is missing.

### Installing

#### Windows 95 / 98 / Me

Windows 9x/Me setup itself runs through the BIOS (INT 13h), so there is no F6-style driver step — install AHCINT9x once Windows is up. Because `AHCINT9X.INF` matches `PCI\CC_010601`, Windows may offer the controller in the **New Hardware Found** wizard on its own; otherwise install it manually:

1. Open **Control Panel → Add New Hardware**, let the wizard continue, and choose to select the hardware from a list.
2. Pick **SCSI controllers**, click **Have Disk**, and point it at `bin\9X\` (`AHCINT9X.INF`).
3. Select **AHCINT9x SATA AHCI Storage Controller** and reboot. `AHCINT9X.MPD` is copied to `WINDOWS\SYSTEM\IOSUBSYS`.

If the protected-mode driver fails to load, Windows 9x falls back to MS-DOS compatibility mode for the affected disks (check **Device Manager → Performance** and `BOOTLOG.TXT`).

> **Windows 95 RTM users:** if the controller shows a **yellow exclamation mark** in Device Manager, the original Windows 95 `SCSIPORT.PDR` is the problem. Either install the [AMDK6 Update](https://winworldpc.com/download/c3a0c2b0-c398-c2a8-3e02-11c3a6e28094), or extract `SCSIPORT.PDR` from that update and copy it to `WINDOWS\SYSTEM\IOSUBSYS` yourself — rename the existing `SCSIPORT.PDR` there to `SCSIPORT.BKP` first so you keep a backup.

#### GUI-mode setup (Windows 2000/XP/XP x64/Server 2003)

Use `bin\2KXP\ahcint.inf` with **"Have Disk"** during a manual driver install. The same INF serves both architectures: it copies `i386\ahcint.sys` on x86 and `amd64\ahcint.sys` on x64. The device shows up as **AHCINT SATA AHCI Storage Controller** (x86) or **AHCINT SATA AHCI Storage Controller (x64)**, and the driver installs as service `AHCINT` under `LoadOrderGroup = SCSI Miniport`.

#### Text-mode (F6) setup — Windows 2000/XP/XP x64/Server 2003

Copy the contents of `bin\2KXP\floppy_i386\` (x86) or `bin\2KXP\floppy_amd64\` (x64) onto a floppy disk (or a virtual floppy image for VM installs), press **F6** at the start of text-mode setup, and select **AHCINT SATA AHCI Storage Controller (ages2001)** — or **AHCINT SATA AHCI Storage Controller x64 (ages2001)** on x64. Required whenever the install disk itself sits behind the AHCI controller.

#### Windows NT 3.1 / 3.50 / 3.51 / NT 4.0

NT uses the older OEM Setup mechanism (`oemsetup.inf`) rather than a standard `.inf`/`.cat` pair. Use **"Have Disk"** during setup (GUI-mode) or the equivalent F6 OEM prompt (text-mode), point it at `bin\NT\floppy\`, and select **AHCINT SATA AHCI Storage Controller (ages2001)**.

**`boot.ini` on Windows NT 3.1:** both ARC paths work. `multi(0)disk(0)rdisk(0)partition(1)\WINNT` makes NTLDR read through the BIOS (INT 13h), so the BIOS has to boot from the AHCI disk. `scsi(0)disk(0)rdisk(0)partition(1)\WINNT` (`disk(n)` = AHCI port number) makes NTLDR drive the controller itself through `NTBOOTDD.SYS`, a copy of `ahcint.sys` in the root of the system partition; this works with NT 3.1's own NTLDR and with the Windows 2000 NTLDR/NTDETECT.COM.

**Disk class driver:** NT 3.x's `scsidisk` (NT 4.0: `disk`) must be a boot driver (`Start = 0`) like the miniport. Otherwise NTLDR does not load it and a system disk on AHCI stops with `0x7B INACCESSIBLE_BOOT_DEVICE`; and a class driver set to start at boot while the miniport started later would not see the AHCI disks at all. Text-mode setup sets this when it installs onto a SCSI disk; `oemsetup.inf` sets it too (`Start = 0` for `scsidisk` and `disk`, where present) when the driver is added through *Windows NT Setup* / *SCSI Adapters*. When moving an existing installation onto the AHCI controller by hand, set `HKLM\SYSTEM\CurrentControlSet\Services\scsidisk\Start` (NT 4.0: `...\disk\Start`) to `0` first, and `AHCINT`'s own `Start` to `0`.

## Configuration

Registry values set at install time on the NT family (`ahcint.inf` service section, `txtsetup.oem` `[Config.scsi.AHCINT]`, `oemsetup.inf`):

- **Tag** – boot-load ordering tag: `40` on 2000/XP/x64, `33` on NT 3.1/3.50/3.51/4.0
- **Group** – `SCSI Miniport`
- **Type / Start / ErrorControl** – `1` (`SERVICE_KERNEL_DRIVER`) / `0` (boot start) / `1` (normal)
- **Event log** – `IoLogMsg.dll` registered as the event message file (`TypesSupported = 7`)
- **2000/XP/x64 only** – `Parameters\PnpInterface\5 = 1` (PnP on the PCI bus). Set by `ahcint.inf`; when it is missing, the driver creates it itself at load (see [Windows 2000/XP/x64 specifics](#windows-2000xpx64-specifics))

On Windows 9x/Me, `AHCINT9X.INF` registers the controller with `DevLoader = *IOS` and `PortDriver = AHCINT9X.MPD` (plus `DontLoadIfConflict = Y`); there are no extra settings to configure.

### Driver parameters

On the NT family the driver reads the ScsiPort argument string, the `REG_SZ` value `DriverParameters` under `HKLM\SYSTEM\CurrentControlSet\Services\AHCINT\Parameters\Device` (create the keys if they are missing; reboot to apply):

| Value | Effect |
|---|---|
| `NCQ=0` | No NCQ: every command is Fast Polling, no interrupt is used (the behaviour of 1.4 and earlier) |
| `NCQ=1` | Windows NT 3.1 only: connect the controller's INTx line and use NCQ (off by default on NT 3.1, see [Windows NT 3.1 specifics](#windows-nt-31-specifics)) |
| `STANDBY=1` | STANDBY IMMEDIATE after the flush at system shutdown (the default up to 1.5.0.0; off since 1.6.0.0, see [Power management](#power-management)) |
| `TRIM=0` | No UNMAP and no TRIM mode: VPD B2h and READ CAPACITY(16) report no logical block provisioning |
| `OFF=list` | Ports the driver leaves alone, separated by commas: `OFF=2,5` turns off ports 2 and 5 of every controller; `p.m` a device behind the port multiplier on port `p` (PM port `m`); a PCI location `bus.device.function:` in front (decimal, as Device Manager shows it) limits an item to one controller: `OFF=0.31.2:4`. A port turned off gets no command and no COMRESET; its DMA engines are stopped and its interrupts disabled (the BIOS may have left them running) |
| `IRQ=0` | Windows NT 3.1–4.0: no interrupt at all, Fast Polling (for a machine whose interrupt routing is wrong; see [Interrupt check](#interrupt-check-nt-35x40)) |
| `RC16=1` / `RC16=0` | READ CAPACITY(10) reports `FFFFFFFFh` for a disk of 2 TB or more (`RC16=1`, for a class driver or filter that then reads READ CAPACITY(16)), or `FFFFFFFEh` (`RC16=0`); the default depends on the Windows version, see [Disks of 2 TB and more](#disks-of-2-tb-and-more) |

Several values are separated by spaces or semicolons.

### Device Manager page

On Windows 2000, XP and Server 2003 (x86 and x64) the controller's properties in Device Manager have a **Ports** tab (`ahcipp.dll`):

- The list shows every port the controller implements, with the device on it (vendor and product from the inquiry data, `Port multiplier, n devices`, or `(no device)`); a port turned off at boot is listed as `(turned off)`.
- Clearing a check box turns that port off: the page writes an `OFF=` item with the controller's PCI location (`bus.device.function:item`) into `DriverParameters`; checking it again removes the item. An `OFF=` item without a location (for every controller) that this controller should no longer follow is replaced by items for each of the other AHCINT controllers, so they keep it. Devices behind a port multiplier (`p.m` items) are listed when `OFF=` names them; to turn one off, type the item into the box.
- The box under the list shows the whole `DriverParameters` string, for all controllers; it can be edited directly (other values, items the list does not show), and the check boxes follow it.
- **OK** or **Apply** stores the string. Device Manager then restarts the controller, which takes effect at once for a controller without the system disk, page file or hibernation file, or asks for a restart of the computer.
- Writing the value needs administrator rights (it is under `HKLM`).

NT 3.1–4.0 have no Device Manager: there `OFF=` goes into the registry by hand, as above. On Windows 9x/Me it goes into `AdapterSettings`.

### Build-time limits

The limits are compile-time constants in `src\ahcint.h`:

```c
#define MAX_SUPPORTED_PORTS             32      /* AHCI ports per controller (AHCI's own limit) */
#define AHCI_MAX_LUNS                   15      /* devices behind one port multiplier */
#define AHCI_MAX_CONTROLLERS            32      /* AHCI controllers per system */
```

**Ports** are addressed by their index in the HBA's `PI` (Ports Implemented) register, so every port of an AHCI controller is reached (earlier builds stopped at port 7 and ignored the ports above). NT 3.1 and Windows 9x take only eight targets per SCSI bus, so the driver puts eight ports on each bus: port `n` is `PathId = n / 8`, `TargetId = n % 8`. Ports 0–7 keep the addresses they always had (bus 0, target = port), and a controller with 8 ports or fewer still reports a single bus. The initiator ID is 8 on every bus, past the last target, so no port is hidden.

What more ports cost, and what is done so that ordinary systems do not pay for it:

- **DMA arena** – sized at run time for the ports `PI` reports (see [Memory Allocation](#memory-allocation)); a port turned off with `OFF=` gets none. With NCQ the arena stays within 160 KB (`AHCI_DMA_BUDGET`); a controller with so many ports that this would leave fewer than 8 tags per port gets 8 tags per port beyond it (32 ports: 281 KB). A 6-port controller gets exactly what it got before.
- **ABAR range** – the driver maps, and on NT 3.x/4.0 claims, only the registers it uses: 500h bytes (ports 0–7) as before, and only on a controller with ports above 7 as far as its last port (1100h for 32 ports). HBAs are often only 4 KB apart, so a fixed larger range would collide with the next controller's ABAR and ScsiPort would reject that controller.
- **Device extension** – the port and device tables sit in a pool at the end of `HW_DEVICE_EXTENSION`: 20 KB (x86) / 25 KB (x64) for 32 ports, 48 devices and the TRIM pattern. The crash dump and hibernation stack has little memory for the miniport's extension (a 38 KB extension of a test build failed with C000009Ah), so the dump instance asks for 3.4 KB (x86) / 4.7 KB (x64): ports 0–7, no port multiplier devices, no TRIM pattern. Hibernation and crash dumps therefore work with the system disk on ports 0–7.
- **Scanning** – ScsiPort scans eight targets on each bus the controller has; loops over the ports skip the ones not implemented.

**`AHCI_MAX_LUNS`**: devices behind a port multiplier become LUN 0, 1, 2, ... of the port's target. Windows 2000 and later are told 15 LUNs per target; NT 3.1–4.0 and 9x scan LUNs 0–7, so they see the first eight devices of a multiplier.

**`AHCI_MAX_CONTROLLERS`** sizes the table that records which PCI bus:slot each driver instance has claimed, so that two instances never drive the same HBA (12 bytes per entry).

After changing a value, rebuild every target you ship (`src\9X`, `src\NT`, `src\2KXP`); they all compile the same `ahcint.h`.

## Debugging

The NT, 2000/XP and x64 variants log via `DbgPrint` (through the `AHCI_DBG` macro), visible through a kernel debugger (e.g. WinDbg / i386kd, depending on target OS) or `DebugView` once a debugger port is attached. Debug output is prefixed `[AHCINT]`.

The 9X variant prints through the VMM's `_Debug_Printf_Service` (the same approach as SweetLow's nvme9x), so messages are fully formatted and go wherever Windows 9x debug output goes: WDEB386 on a serial port, SoftICE, or DebugView for Windows 9x. The calls are only compiled into `DEBUG=1` builds (`set DEBUG=1` before `build.bat`); release builds carry no debug code. Messages are prefixed `[ahcint9x]`.

In code, all targets use one macro with double parentheses (no variadic macros needed for VC++ 2.0 / MSVC 4.0):

```c
AHCI_DBG((AHCI_PFX "Port %lu: TFD=%08lX\n", port, tfd));
```

## Technical Notes

### I/O Model

- **Fast Polling (all targets):** a command is issued from `HwStartIo` in slot 0 and polled every 10 µs on `PxCI`/`PxIS` (see [Command timeout](#command-timeout)), then completed before `HwStartIo` returns. Completion never depends on an interrupt being delivered — this also covers text-mode Setup, where the HAL has not assigned an IRQ, and the crash dump stack. The loop exits as soon as `PxIS.TFES` or a fatal HBA error is set, so an empty CD tray answers NOT READY at once; the port is then stopped and restarted as the AHCI spec requires. Errors are taken from `PxIS.TFES`, not from `PxTFD.ERR` alone: after a failed command `PxTFD` keeps the old ERR bit until the next command's first FIS arrives.
- **NCQ (NT 3.1 to XP x64):** disk reads and writes go out as READ/WRITE FPDMA QUEUED and `HwStartIo` returns at once; see [NCQ](#ncq). Before any Fast Polling command on a port, the queued commands there are drained.

### NCQ

NCQ is used for a disk when the HBA reports `CAP.SNCQ`, IDENTIFY word 76 bit 8 is set, an interrupt is connected, the driver is not in the crash dump stack and the disk is not behind a port multiplier. The queue depth is the smaller of the disk's (word 75) and the HBA's slot count (`CAP.NCS`), limited by the DMA budget. INQUIRY then reports `CmdQue`, and ScsiPort sends several requests per LU (`TaggedQueuing`, `MultipleRequestPerLu`); as long as a port has free tags the driver asks for the next request at once (`NextLuRequest`).

- **Completion:** `HwInterrupt` reads `PxSACT` and completes every tag whose bit the device cleared (SDB FIS). FUA requests set the FUA bit in the FIS.
- **Missing interrupts:** while commands are outstanding a 100 ms ScsiPort timer (`RequestTimerCall`) also checks the ports. If it finds completed commands that no interrupt reported five times, the driver drains all queues, turns the interrupts off and continues with Fast Polling for good. A wrong IRQ routing therefore costs speed, not data.
- **Errors:** on `PxIS.TFES` the port is stopped and restarted, the failed tag is read from the NCQ Command Error log (READ LOG EXT page 10h) and completed with sense data from its status/error; all other outstanding commands are returned with `SRB_STATUS_BUSY` and ScsiPort sends them again. If the log cannot be read and only one command was outstanding, that one failed. A device that keeps failing in a way the driver cannot attribute gets NCQ turned off after three such errors.
- **Turning it off:** `NCQ=0` in `DriverParameters` (see [Driver parameters](#driver-parameters)).

### Port multipliers

On HBAs with `CAP.SPM` every port is first reset with a software reset to PM port 15. A port multiplier answers with its signature (`96690101h`); the driver then sets `PxCMD.PMA`, reads the number of device ports (GSCR 2), brings each link up through its PSCR SControl/SStatus registers (READ/WRITE PORT MULTIPLIER, E4h/E8h), resets each device with a software reset and IDENTIFYs it. The devices found become LUN 0, 1, 2, ... of the port's target ID, in PM port order (up to `AHCI_MAX_LUNS` = 15; NT 3.1–4.0 and 9x see LUNs 0–7). Commands carry the PM port number in the command header and FIS (command-based switching), one at a time.

Without a multiplier the device answers the reset to port 15 with its own signature and is used as before. The crash dump stack does not enumerate multipliers: commands go to PM port 0, so a system disk behind a multiplier can be hibernated to only when it sits on PM port 0.

### Power management

- **Shutdown and flush:** `SRB_FUNCTION_FLUSH` issues FLUSH CACHE (EXT) to every disk of the target (only when the write cache is present); SYNCHRONIZE CACHE does the same for one disk. `SRB_FUNCTION_SHUTDOWN` flushes every disk of the target; with `STANDBY=1` it then sends STANDBY IMMEDIATE, so the disk parks its heads (or an SSD saves its tables) before the power goes instead of an emergency power-off. Off by default: 1.4.0.0 sent nothing at shutdown, and the reported hang at shutdown on XP x64 came with 1.5.0.0. The driver declares `CachesData`, without which ScsiPort passes neither request.
- **START STOP UNIT:** stop = flush, then STANDBY IMMEDIATE; start = IDLE IMMEDIATE. The next read or write spins the disk up again.
- **Windows 2000/XP/x64 — `HwAdapterControl`:** `ScsiStopAdapter` (sleep, hibernation, device removal) drains the queues, flushes and puts the disks in standby, stops the ports and disables the interrupts; `ScsiRestartAdapter` sets the HBA up again: ports, devices behind port multipliers, the MODE SELECT settings (write cache, look-ahead).
- **Hibernation and crash dump (2000/XP/x64):** the dump stack loads the miniport a second time without a driver object (or with `DUMP=` in the argument string). That instance uses Fast Polling only, no queued requests and no port multiplier enumeration, and keeps its device extension small (see [Build-time limits](#build-time-limits)).
- **Lost HBA state:** before each request the driver checks that the port still runs with its command list (`PxCMD.ST`/`FRE`, `PxCLB`, `GHC.IE` with NCQ). If a sleep state, the BIOS or another driver reset the HBA, it is set up again first; outstanding requests are returned as `SRB_STATUS_BUS_RESET` and retried by ScsiPort. On Windows 9x `HwAdapterState` (restore) triggers the same check.
- **NT 3.1–4.0 and 9x:** these ScsiPorts have no power callbacks. An ACPI or APM driver can use the `AHCINT` IOCTL (below): `POWER_DOWN` before the system enters a sleep state, `POWER_UP` (or simply the next request) after resume.

### IOCTL interface

Both are `IOCTL_SCSI_MINIPORT` requests (an `SRB_IO_CONTROL` header followed by the data) sent to `\\.\ScsiN:` (or, on 2000/XP, to the disk):

| Signature | ControlCode | Function |
|---|---|---|
| `"AHCINT  "` | `1` GET_INFO | Returns `AHCI_IOCTL_INFO`: driver version (`01050100h`), `CAP`, `PI`, flags (bit 0 NCQ active, bit 1 powered down, bit 3 TRIM mode), tags per port, number of devices per port |
| `"AHCINT  "` | `20h` TRIM_MODE_ON | A 4 KB pattern follows the header: from now on a write whose data is that pattern repeated is not written but trimmed (see [TRIM](#trim)) |
| `"AHCINT  "` | `21h` TRIM_MODE_OFF | Back to normal writes |
| `"AHCINT  "` | `10h` POWER_DOWN | Drain, flush, STANDBY IMMEDIATE on every disk, stop all ports, disable interrupts. GET_INFO does not wake the HBA; any other request does |
| `"AHCINT  "` | `11h` POWER_UP | Set the HBA up again (as after `ScsiRestartAdapter`) |
| `"SCSIDISK"` | `1B0500h`–`1B050Ch` | SMART, as `IOCTL_SCSI_MINIPORT_SMART_VERSION` ... `_WRITE_LOG` (`NTDDSCSI.H`): IDENTIFY, READ ATTRIBUTES/THRESHOLDS, ENABLE/DISABLE, RETURN STATUS, AUTOSAVE, SAVE ATTRIBUTES, OFFLINE DIAGS, READ/WRITE LOG. `bDriveNumber` is the port; the device map in SMART_VERSION follows `atapi.sys` (bit n ATA, bit n+4 ATAPI, ports 0–3) |

The structures are in `src\ahcint.h` (`AHCI_SRB_IO_CONTROL`, `AHCI_IOCTL_INFO`, `AHCI_SENDCMD_IN/OUT`).

### TRIM

- **UNMAP:** every block descriptor becomes TRIM ranges of DATA SET MANAGEMENT (06h, 48-bit DMA, up to 65535 blocks per range, 64 ranges per 512-byte block); the command is issued like FLUSH (not queued). Offered only on devices whose IDENTIFY word 169 bit 0 reports TRIM; VPD B0h gives 64 descriptors and the maximum LBA count, VPD B2h LBPU. LBPRZ (zeroes after TRIM) follows IDENTIFY word 69 bits 14 and 5.
- **TRIM mode:** Windows versions before 7 never send TRIM. `TRIM_MODE_ON` gives the driver a 4 KB pattern; while the mode is on, a write whose data is that pattern repeated over the whole request is trimmed instead of written. `tools\trimnt.exe <drive letter>` switches the mode on with a random pattern on every AHCINT, IDENT, MMCSDNT and NVMENT adapter, fills the free space of the volume with files of the pattern (unbuffered, write-through), switches the mode off and deletes the files. Other writes are written normally all the time.

### ATA command selection

`AhciParseIdentify` reads IDENTIFY DEVICE once per disk. 48-bit addressing is used only when word 83 is valid (bits 15:14 = `01`), has bit 10 (48-bit Address feature set) set, and words 100–103 hold a capacity — the same rule as Linux's `ata_id_has_lba48()`. Capacity then comes from words 100–103, otherwise from words 60–61.

Each read/write is checked against that capacity (out of range → CHECK CONDITION, ILLEGAL REQUEST / LBA OUT OF RANGE) and sent as READ/WRITE DMA (28-bit) when it ends below LBA 2^28 and moves at most 256 sectors; only otherwise, and only on a 48-bit device, as READ/WRITE DMA EXT. A zero transfer length in READ/WRITE(10)/(16) completes without touching the disk (ATA would read a count of 0 as 256 / 65536 sectors).

LBAs are kept as two `ULONG`s (`AHCI_LBA`), so no `__int64` arithmetic — and none of the `_aulldiv`/`_allshl` CRT helpers the 9x build does not link — is needed.

### Logical sector size

ATA disks may use logical sectors larger than 512 bytes ("4Kn" drives use 4096). `AhciParseIdentify` reads word 106: when it is valid (bits 15:14 = `01`) and bit 12 is set, words 117–118 give the logical sector size in words. 512, 1024, 2048 and 4096 bytes are taken; anything else leaves the port unusable instead of guessing. All ATA LBAs and sector counts are in logical sectors, so the SATL only has to report the right block length (READ CAPACITY(10)/(16), MODE SENSE block descriptor) and size each transfer as count × sector size. When bit 13 is set, bits 3:0 (logical sectors per physical sector) go into READ CAPACITY(16) byte 13, so 512e drives report their 4 KB physical sectors.

What the operating system does with such a disk is up to its class and file system drivers. Tested with QEMU patched to emulate ATA disks with large logical sectors (see `TESTS.TXT`):

| OS | 1024 bytes | 2048 bytes | 4096 bytes |
|---|---|---|---|
| Windows NT 3.1 | FAT, NTFS | NTFS (FAT formats but does not mount) | NTFS (FAT formats but does not mount; NT 3.1's FORMAT.COM faults at the end of an NTFS format, the volume is fine) |
| Windows NT 4.0 SP6 | FAT, NTFS | FAT, NTFS | FAT, NTFS |
| Windows 2000 | FAT, NTFS | FAT, NTFS | FAT, NTFS |
| Windows 95 | FAT | FAT | Not usable: Windows 95 reads the MBR and boot sector, then fails every access to the drive ("General failure") without sending a read |

### Windows 9x specifics

- **ABAR mapping:** `ScsiPortGetDeviceBase` can fail to return a usable linear address for a memory-mapped PCI BAR on Windows 95 (Microsoft KB Q169584). The 9X build maps the ABAR with the VMM's `_MapPhysToLinear` service instead, as non-cached (`MPL_NonCached`), so register polling always sees live hardware state.
- **`HwAdapterState`:** `SCSIPORT.PDR` calls this for PCI miniports during Plug and Play state changes; the 9X build provides it, because leaving it `NULL` makes `SCSIPORT.PDR` report "Init Failure" for the miniport. A restore makes the next request check whether the HBA lost its state (see [Power management](#power-management)).
- **No NCQ:** `SCSIPORT.PDR` sends one request per device at a time, so the 9X build stays on Fast Polling.
- **PCI discovery:** like the NT targets, the bus:slot `SCSIPORT.PDR` hands in is tried first, then the PCI bus-scan fallback.

### Windows NT 3.1 specifics

NT 3.1's ScsiPort and HAL predate PCI support in non-PnP miniports. The NT build handles this at run time, so the same `ahcint.sys` runs on NT 3.1, 3.50, 3.51 and 4.0:

- **`HW_INITIALIZATION_DATA` size:** NT 3.1's `ScsiPortInitialize` accepts only its own 40h-byte structure and fails anything else before calling `HwFindAdapter`. `DriverEntry` first calls it with the NT 3.50–4.0 size on `PCIBus`; if that fails without `HwFindAdapter` ever being called, it retries with the 40h-byte layout on the ISA bus.
- **PCI configuration space:** NT 3.1's `HalGetBusData` only knows CMOS and EISA data, so `ScsiPortGetBusData(PCIConfiguration)` returns nothing. The driver then uses configuration mechanism #1 (ports `CF8h`/`CFCh`) for the bus scan. `ScsiPortSetBusDataByOffset` does not exist on NT 3.1, so on the whole NT build the PCI command register is written through the same ports, and only when the BIOS left memory space or bus mastering off.
- **`PORT_CONFIGURATION_INFORMATION`:** 58h bytes on NT 3.1, without `SlotNumber` and `MaximumNumberOfTargets` (the access ranges follow right after it). The driver checks `ConfigInfo->Length` before touching those fields.
- **DMA:** NT 3.1's HAL gives an ISA bus master map registers below 16 MB in every case (it ignores `Dma32BitAddresses`), so with more RAM every transfer is bounced, and the copy-back overwrites the data the SATL itself writes into the request (INQUIRY, READ CAPACITY, ...): scsidisk sees garbage and the boot ends in `0x7B`. Before taking the uncached extension the driver therefore reports `Eisa` as the interface type; the HAL gives a scatter/gather master on EISA no map registers, and the HBA sees real physical addresses. Tested up to 3 GB RAM.
- **Interrupt:** NT 3.1 has no PCI bus, so ScsiPort does not look up the HBA's interrupt. By default no interrupt is reported (level 0), ScsiPort connects none, and the driver uses Fast Polling. With `NCQ=1` in `DriverParameters` the driver reports the INTx line from config space (level-sensitive, on the EISA bus it already reports for DMA) and uses NCQ; if those interrupts do not arrive, the watchdog goes back to Fast Polling (see [NCQ](#ncq)). It stays opt-in because a failed interrupt connection on NT 3.1 fails the whole adapter, and with it the boot.
- **Several controllers:** each HBA asks to be called again (`*Again = TRUE`); the next call scans for the next unclaimed HBA.

### Interrupt check (NT 3.5x/4.0)

On NT 3.50–4.0 the driver picks the HBA's PCI function itself, so ScsiPort does not look up its interrupt; the driver reports the INTx line from configuration space. Before it does, it asks the HAL for a vector (`HalGetInterruptVector`, as ScsiPort will). An MPS HAL (`halmps.dll`) translates PCI interrupts through the MP table, and a modern BIOS often writes no entry there for the SATA controller (or for anything behind a PCI-PCI bridge): the HAL then returns no vector, ScsiPort cannot connect the interrupt and fails the whole adapter, and with the system disk on it the boot stops with `0x7B INACCESSIBLE_BOOT_DEVICE` (GitHub issue #2: NT 3.51 with `halmps.dll` on an X99 board). The driver now reports no interrupt in that case and runs the HBA with Fast Polling (no NCQ); the debug output says `no vector for IRQ level n ... Fast Polling`. `IRQ=0` does the same unconditionally. On 2000/XP/x64 the PnP manager assigns the interrupt; on NT 3.1 and 9x none is reported by default.

### Disks of 2 TB and more

A disk whose last LBA is `FFFFFFFFh` or beyond does not fit READ CAPACITY(10). The SCSI answer is `FFFFFFFFh`, which tells the class driver to read READ CAPACITY(16). The disk class drivers of Server 2003 SP1 and XP x64 (and later) do; those of NT 3.1–4.0, Windows 9x, 2000 and XP x86 do not: they add one to the value in 32 bits and see a disk of 0 bytes. The driver therefore answers `FFFFFFFEh` (2 TB minus one sector) where the class driver does not know READ CAPACITY(16): those systems see and use the first 2 TB, which is all they can address with READ(10)/WRITE(10) and an MBR anyway. READ CAPACITY(16) and 16-byte CDBs always give the whole disk.

The choice is made once at load time: the NT and 9x builds always clip; the 2000/XP x86 build asks `PsGetVersion` (Server 2003 with a service pack, or later, reads READ CAPACITY(16)); the x64 build never clips. `RC16=1` / `RC16=0` in `DriverParameters` override it (for example `RC16=1` on 9x with a layer such as LBA64HLP.VXD that reads READ CAPACITY(16) itself).

### Windows 2000/XP/x64 specifics

- **Text-mode setup (F6):** the setup kernel boots with a bare `AHCINT` service key; the `[Config.scsi.AHCINT]` values of `txtsetup.oem` are written only to the installed system. Without `Parameters\PnpInterface\5`, the XP x64 / Server 2003 x64 ScsiPort fails `IRP_MN_START_DEVICE` before `HwFindAdapter` is called, so setup stops with STOP 0x7B (the x86 ScsiPort tolerates the missing value). `DriverEntry` therefore creates `Parameters\PnpInterface\5 = 1` under its service key when the `PnpInterface` key does not exist yet, before `ScsiPortInitialize`. An existing key is left as it is. Both F6 `txtsetup.oem` files also set the value in `[Config.scsi.AHCINT]`, so a system installed through F6 gets it from setup as well.

### Command timeout

A Fast Polling command is polled until the SRB's `TimeOutValue` minus one second (10 s when the SRB carries none, at most 60 s); commands the driver issues on its own (ATAPI REQUEST SENSE, MODE SENSE translation) get 3 s. Earlier builds used a fixed 3 s for everything, which was wrong both ways:

- **Too short:** one write can legitimately take several seconds — disk spin-up, a dynamically expanding VHD growing under a benchmark's large test file, host cache flushes in a VM. The driver then gave up, stopped the port while the HBA was still transferring and failed the request, so long runs such as CrystalDiskMark could not finish (in QEMU the emulator itself crashed on the mid-transfer stop). Disk class drivers use a 10 s `TimeOutValue`, so a write now has 9 s.
- **Too long for short SRBs:** ScsiPort runs its own one-second request timer. On a multiprocessor system that timer fires on another CPU while `HwStartIo` is still polling; if the poll outlasts the SRB's `TimeOutValue`, ScsiPort starts its own timeout handling (bus reset) for a request the miniport is about to complete. Ending the poll one second early keeps the miniport's timeout ahead of ScsiPort's.

Flush, standby and the power IOCTL use 30 s. NCQ commands are timed by ScsiPort itself (the SRB's `TimeOutValue`); when it expires, ScsiPort resets the bus and the driver returns every outstanding command.

When a command does time out, the port restart issues a COMRESET if the command engine will not stop or the device still reports BSY/DRQ, and sets `PxCMD.ST` again only after BSY/DRQ have cleared (AHCI 1.3, 10.4.2 and 10.3.1).

### One source tree

`src\ahcimain.c`, `src\ahcicmd.c`, `src\ahcisatl.c` and `src\ahcint.h` build every target. The build files set the target:

| Define | Target | What it changes |
|---|---|---|
| `AHCI_W9X` | Windows 95/98/Me | ABAR mapped with VMM `_MapPhysToLinear` (KB Q169584); `HwAdapterState`; no NCQ; debug output via VMM `_Debug_Printf_Service`; implies `AHCI_NT4` |
| `AHCI_NT4` | NT 3.1/3.50/3.51/4.0 (and 9x) | No HBA reset (links taken over as the BIOS left them); ATAPI packets always carry the DMA bit; no `Dma64BitAddresses` in `PORT_CONFIGURATION_INFORMATION`; access range and INTx reported to ScsiPort by the miniport; no auto-sense (the class driver issues REQUEST SENSE) |
| `AHCI_NT31_COMPAT` | NT build only (set by `ahcint.h` for `AHCI_NT4` without `AHCI_W9X`) | Windows NT 3.1 support in the same binary, see [Windows NT 3.1 specifics](#windows-nt-31-specifics) |
| none | 2000/XP/2003 x86 and x64 | HBA reset (`GHC.HR`) at init; 64-bit DMA when `CAP.S64A`; PnP-assigned interrupt; auto-sense; `HwAdapterControl`; crash dump / hibernation |

The 2000/XP x86 and XP x64/Server 2003 x64 drivers differ only in the build environment; there are no architecture-specific code paths.

### Memory Allocation

- **DMA arena** – one uncached allocation per controller, sized in `HwFindAdapter` for the ports `PI` reports (without those turned off with `OFF=`): per port a 1 KB command list, a 256-byte received-FIS area, a 512-byte bounce buffer and one 768-byte command table per slot (one with Fast Polling, one per NCQ tag), plus alignment. With 6 ports that is 22 KB with Fast Polling and 157 KB with 31 tags; the total is kept within 160 KB (`AHCI_DMA_BUDGET`) by reducing the tags per port
- **PRDT** – up to 40 entries per command table; ScsiPort is told 32 physical breaks and 128 KB per I/O, so an unaligned 128 KB buffer (33 pages) still fits
- **Bounce buffer** – 512 bytes per port in the DMA arena, used for IDENTIFY, READ LOG EXT and the ATAPI REQUEST SENSE / MODE SENSE commands the driver issues itself

## Version history

### 1.6.0.0 — 2026-10-19

- ATAPI devices other than CD/DVD drives (Iomega ZIP, LS-120/SuperDisk, MO): INQUIRY reported every ATAPI device as a CD-ROM, so a removable ATAPI disk got a CD-ROM drive letter (found with a ZIP 100 on IDENT). The device type now comes from IDENTIFY PACKET DEVICE (word 0 bits 12:8, removable bit 7): such a device is a removable disk. For those, READ(6)/WRITE(6) are sent as READ(10)/WRITE(10), which ATAPI devices understand, and the MODE SENSE(6) translation now keeps the device-specific parameter (write protect of the medium), the block descriptor length and the mode data length, and asks the drive for the whole requested length.
- Device Manager page (**Ports** tab, `ahcipp.dll`) for 2000/XP/x64: ports turned off with check boxes instead of `OFF=` typed into the registry; installed by `ahcint.inf` and the F6 disks.
- Windows XP x64 / Server 2003 x64: the HBA registers are read and written through ScsiPort's exported functions, as in the x86 build. The x64 build used inline accesses without a compiler barrier, so the compiler could move accesses to the command lists and received-FIS areas across them. This was the one x64-only difference found while looking into a reported hang at shutdown on XP x64; the hang itself could not be reproduced in QEMU (see `TESTS.TXT`).
- System shutdown flushes the write cache only, as before NCQ and power management came in: STANDBY IMMEDIATE on `SRB_FUNCTION_SHUTDOWN`, new in 1.5.0.0, is now opt-in (`STANDBY=1`). 1.4.0.0, which sent nothing at shutdown, had no reported hang on XP x64; this is the shutdown-only change between the two.
- NT 3.50–4.0 with an MPS HAL (GitHub issue #2, NT 3.51 on an X99 board, `0x7B` at boot): when the HAL has no vector for the HBA's interrupt (no MP table entry for it), the driver now runs it with Fast Polling instead of letting ScsiPort fail the adapter. `IRQ=0` turns the interrupt off by hand. See [Interrupt check](#interrupt-check-nt-35x40).
- All 32 ports of an AHCI controller (8 before) and up to 32 controllers (8 before); ports above 7 are on SCSI buses 1–3, eight per bus, ports 0–7 keep their addresses. Up to 15 devices behind a port multiplier on Windows 2000 and later. Memory and mapping grow only with the ports a controller has; the crash dump / hibernation instance needs less than before (3.4 KB extension).
- `OFF=` turns off single ports, or devices behind a port multiplier, optionally on one controller only (by its PCI location).
- Disks of 2 TB and more: READ CAPACITY(10) reported `FFFFFFFFh`, which the class drivers of NT 3.1–4.0, 9x, 2000 and XP x86 turned into a disk of 0 bytes; they now get `FFFFFFFEh` and see the first 2 TB. XP x64 and Server 2003 SP1 and later still get `FFFFFFFFh` and read the full size with READ CAPACITY(16). `RC16=0/1` overrides.
- An HBA whose registers read all ones (powered off or removed) is no longer set up again: requests fail at once instead of running through the timeouts of a full re-initialization, and flush and shutdown requests complete without commands.

### 1.5.0.0 — 2026-10-12

- Added NCQ (Native Command Queuing): up to 32 commands per port, completed by interrupt, on Windows NT 3.50 to XP x64 (NT 3.1 with `NCQ=1`). A watchdog falls back to Fast Polling when interrupts are not delivered; `NCQ=0` turns NCQ off.
- Added port multiplier support: every disk behind a port multiplier is a LUN of its port.
- Added TRIM support for SSDs (SCSI UNMAP as ATA DATA SET MANAGEMENT), VPD pages B0h/B1h/B2h and logical block provisioning in READ CAPACITY(16).
- Added TRIM mode and the `trimnt` tool, which trims the free space of a volume on Windows versions that send no TRIM.
- Added SMART through the "SCSIDISK" IOCTL (as `atapi.sys`), ATA PASS-THROUGH(12/16), and LOG SENSE pages for temperature and SMART status.
- Added FUA, MODE SELECT (write cache and read look-ahead), START STOP UNIT, REPORT LUNS and VERIFY(12).
- Added power management: flush and standby at system shutdown and on power transitions, hibernation and crash dump support on Windows 2000/XP, re-initialization when the controller lost its state, and a power-down/up IOCTL for NT 3.1–4.0 and 9x.
- Fixed SATA links left at a lower speed by the firmware: every port is renegotiated to the fastest speed both ends support.
- Fixed a port whose device does not answer IDENTIFY: it is now reported as empty instead of as a disk with no capacity.
- Fixed a command after a failed command failing as well (stale error status).
- Fixed the "Files Needed" prompt for `ahcint.sys` in GUI-mode setup after an F6 installation on XP x64 (the F6 disks now carry a flat INF).
- `oemsetup.inf` now also makes the disk class driver a boot driver on NT 3.1–4.0.
- Added version information to the driver files (file properties show 1.5.0.0).

### 1.4.0.0 — 2026-10-02

- Added Windows NT 3.1 support (the same `ahcint.sys` runs on NT 3.1, 3.50, 3.51 and 4.0).
- Added support for disks with 1024, 2048 and 4096-byte logical sectors.
- Fixed a slow disk being reset in the middle of a transfer: Fast Polling now waits as long as the request's own timeout allows.
- Fixed a race with ScsiPort's request timer on multiprocessor systems.
- Fixed STOP 0x7B in XP x64 text-mode setup with the driver loaded from the F6 disk.

### 1.3.0.0 — 2026-09-29

- All targets (Windows 9x, NT and 2000/XP x86/x64) are built from one source tree.
- Added 16-byte CDBs on every target: READ/WRITE/VERIFY(16), READ CAPACITY(16) and SYNCHRONIZE CACHE(16), for disks past 2 TB.
- ATA disks report SPC-3 in INQUIRY and their serial number in VPD page 80h.
- 28-bit commands whenever the request fits; 48-bit commands only on devices that support them.

### 1.2.0.0 — 2026-09-25

- Added Windows 95, 98 and Me support.
- Fixed CD/DVD support.
- Windows 2000 and XP x86 and x64 are built from the same source code.

### 1.1.0.0 — 2026-09-21

- Fixed 0x0000007B BSOD on the ACPI Uniprocessor and ACPI Multiprocessor HALs of Windows 2000 and XP x86.
- Added support for up to 8 AHCI controllers.
- Increased the maximum number of ports from 6 to 8 on NT 3.50, 3.51 and 4.0.

### 1.0.0.0 — 2026-09-16

- Initial release.

## Related drivers

AHCINT, IDENT, MMCSDNT and NVMENT are one family: the same ScsiPort miniport design, SCSI translation layer, IOCTL conventions, Fast Polling fallback and TRIM mode, built from one source tree each for every system from Windows 95 and NT 3.1 to XP x64.

| Driver | Storage |
|---|---|
| **AHCINT** | SATA disks and optical drives on AHCI controllers |
| **IDENT** | PATA disks and optical drives, and SATA controllers in IDE mode |
| **MMCSDNT** | SD/SDHC/SDXC cards and MMC/eMMC on SD host controllers (SDHCI) |
| **NVMENT** | NVMe SSDs (based on [nvme2k](https://github.com/techomancer/nvme2k) by DominBear) |

`tools\trimnt.exe`, shipped with each of them, trims the free space of a volume on any of the four.

## License

Copyright (c) 2026 ages2001. All rights reserved.

**USE AT YOUR OWN RISK.** This driver talks directly to storage controller hardware on operating systems no longer supported or patched by Microsoft. Not recommended for any system holding data you can't afford to lose.

## Acknowledgments

- The AHCI 1.x and ATA/ATAPI-8 specifications
- Windows 95 DDK and Windows NT4/2000/XP DDK documentation

## Special Thanks

- infuscomus for porting the driver to x64
- Dietmar for Fast Polling idea
- DominBear for the [nvme2k](https://github.com/techomancer/nvme2k) project
- SweetLow for [nvme9x](https://github.com/LordOfMice/Tools) (Windows 9x VMM debug output and `_MapPhysToLinear` technique)
- [UniATA project](http://alter.org.ua/soft/win/uni_ata/)
- Windows 2000 Dev Community
- Testers
- And everyone who supports it
