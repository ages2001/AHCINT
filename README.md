# AHCINT - SATA AHCI Driver for Windows NT/2000/XP

An AHCI (Advanced Host Controller Interface) SATA storage controller driver for the Windows NT family, targeting operating systems from Windows NT 3.50 up through Windows XP x64 Edition / Server 2003 x64 — long before Microsoft shipped a native AHCI driver.

## Overview

AHCINT is a SCSI miniport driver built on the classic `ScsiPort` framework. It talks directly to AHCI HBA/port registers (command lists, PRDTs, FIS structures) and translates SCSI requests from the Windows storage stack into ATA/ATAPI commands, so SATA AHCI controllers can be used as boot and data devices on operating systems that predate AHCI entirely.

### Why?

Because plenty of real hardware only exposes its SATA ports in AHCI mode, and NT/2000/XP have no idea what that is.

## Features

- **AHCI 1.0-class hardware support**
  - HBA generic registers (GHC, IS, PI, CAP, CAP2) and per-port registers (CLB, FB, IS, IE, CMD, TFD, SIG, SSTS, SCTL, SERR, CI)
  - Command List / Command Header / PRDT / FIS-based command submission
  - Port state machine handling: COMRESET via `SCTL.DET`, FRE/ST start-stop sequencing, Command List Override (CLO) fallback when a port won't idle
  - BIOS/OS Handoff Control (BOHC) so the driver takes ownership cleanly from the firmware

- **SCSI-to-ATA Translation Layer (SATL)**
  - INQUIRY, REQUEST SENSE, TEST UNIT READY, SYNCHRONIZE CACHE, VERIFY, MEDIUM REMOVAL, READ CAPACITY(10), MODE SENSE(6)
  - READ/WRITE(6) and READ/WRITE(10) on all targets, issued as 48-bit LBA ATA commands (READ/WRITE DMA EXT); READ/WRITE(16) with the full LBA48 address range exposed to SCSI on the 2000/XP and XP x64 targets
  - ATAPI (PACKET command) pass-through for CD/DVD-class devices, alongside plain ATA disk support
  - IDENTIFY DEVICE / IDENTIFY PACKET DEVICE based device enumeration, with cached string extraction for INQUIRY vendor/product fields

- **Multi-target support**
  - Windows NT 3.50 / 3.51 / NT 4.0 (x86) — synchronous, polling-based I/O
  - Windows 2000 / XP (x86) — asynchronous, interrupt-driven I/O
  - Windows XP x64 Edition / Server 2003 x64 (amd64) — asynchronous, interrupt-driven I/O with proper 64-bit pointer handling and SMP memory barriers

## Architecture

```
Application Layer
       ↓
  SCSI Disk Driver
       ↓
   ScsiPort.sys
       ↓
   ahcint.sys  ← This driver
       ↓
  AHCI Controller (PCI, class code 01/06/01)
       ↓
  SATA disk / ATAPI optical drive
```

### Key Components

- **ahci_main.c** – Driver entry, adapter/port initialization, `HwStartIo`, `HwInterrupt`, `HwResetBus`, DMA resource allocation
- **ahci_satl.c** – SCSI command dispatch, SCSI↔ATA/ATAPI translation, INQUIRY/MODE SENSE synthesis, PRDT construction
- **ahcint.h** – AHCI register layout, command/FIS structures, device extension, shared constants
- **ahcint.inf** / **oemsetup.inf** – Installation files (see [Installing the driver](#installing-the-driver))

## Feature matrix by target

| | NT 3.50/3.51/4.0 (x86) | 2000/XP (x86) | XP x64/Server 2003 x64 (amd64) |
|---|---|---|---|
| I/O model | Synchronous (polls `PxCI` in `HwStartIo`) | Asynchronous (`HwStartIo` returns pending, completion in `HwInterrupt`. And FAST_POLL fallback) | Asynchronous, same model as 2000/XP |
| Max AHCI ports | 8 | 8 | 8 |
| Max AHCI controllers | 8 | 8 | 8 |
| READ/WRITE(16), full LBA48 range | Partial — READ/WRITE(10) only, LBA48 *commands* used on the wire but SCSI layer only ever passes the low 32 bits | Yes | Yes |
| REQUEST SENSE | No | Yes | Yes |
| ATAPI (PACKET) support | Yes | Yes | Yes |
| 64-bit addressing (S64A / 64-bit BAR) | No — 32-bit DMA only | Yes | Yes, plus 64-bit BAR high-dword read |
| Hot-plug / late device detection | No | No | Yes — re-checks `PxSSTS` on INQUIRY |
| PRDT entries per command | 32 | 32 | 32 |
| DMA arena size | 64 KB (shared across ports) | 64 KB (shared across ports) | 64 KB (shared across ports) |
| Max transfer size per I/O | ~128 KB | ~128 KB | ~128 KB |
| Interrupt model | Legacy INTx only | Legacy INTx only | Legacy INTx only |

## Known Limitations

- No NCQ (Native Command Queuing) — `PxSACT` is defined on the 2000/XP and x64 targets but never used; every port issues one command at a time via slot 0
- No MSI/MSI-X interrupt support — legacy line-based (INTx) interrupts only, consistent with the AHCI/PCI conventions of the era this driver targets
- ~128 KB maximum transfer size per request (32 PRDT entries against a shared 64 KB DMA arena), well short of AHCI's theoretical per-PRDT 4 MB limit
- No hot-plug support on the NT and 2000/XP targets; the XP x64/Server 2003 x64 target only re-checks device presence opportunistically, on INQUIRY
- No power management (no device sleep/spin-down handling, no S3/S4 resume path beyond what ScsiPort provides)
- Single-queue-depth design — no per-port command queuing beyond the one in-flight command AHCINT itself tracks
- NT 3.50/3.51/4.0 build lacks REQUEST SENSE and READ/WRITE(16); it is the least capable of the three targets by design, matching the OS-era SCSI class driver's expectations
- NT 3.50/3.51/4.0 build issues LBA48 ATA commands (READ/WRITE DMA EXT) but only exposes READ/WRITE(10) to SCSI, whose CDB carries a 32-bit LBA — so addressing tops out at 2^32 sectors (~2 TB at 512 bytes/sector) rather than the full 48-bit range, even though the on-the-wire ATA command supports it

## Building from source

### Requirements

- **NT 3.50 / 3.51 / NT 4.0 (x86):** Windows NT 4.0 DDK + Visual C++ 4.0 (MSVC 4.0)
- **Windows 2000 / XP (x86):** Windows Server 2003 SP1 DDK ("WDK 6001", build 3790.1830)
- **Windows XP x64 / Server 2003 x64 (amd64):** A DDK/WDK with an amd64 ("WNET") cross-compiler — Windows Server 2003 SP1 DDK ("WDK 6001", build 3790.1830), Windows Server 2003 R2 DDK, or the Windows 7 WDK (WinDDK 7600.16385.1)

### Build Instructions

#### For NT 3.50 / 3.51 / NT 4.0 (x86)

```bat
cd DRIVERPATH

set MSVCDIR=C:\MSDEV
set DDKDIR=C:\NT4DDK

set PATH=%MSVCDIR%\BIN;%DDKDIR%\BIN;%PATH%
set INCLUDE=%DDKDIR%\inc;%DDKDIR%\src\storage\inc;%DDKDIR%\inc\crt;%MSVCDIR%\INCLUDE;C:\HYBRIDST\inc
set LIB=%DDKDIR%\lib\i386;%MSVCDIR%\LIB;C:\NT4DDK\lib\i386\free;%LIB%

del *.obj ahcint.sys
cl -nologo -c -Gz -Ox -W3 -Zp8 -Zi -D_X86_=1 -Di386=1 -DCONDITION_HANDLING=1 -DNT_UP=1 -DNT_INST=0 -DWIN32=100 -D_NT1X_=100 -DWINNT=1 -D_WIN32_WINNT=0x0350 /I..\inc ahci_main.c ahci_satl.c
link -nologo -debug -debugtype:both -subsystem:native,3.50 -entry:DriverEntry@8 -driver -base:0x10000 -align:0x200 -out:ahcint.sys ahci_main.obj ahci_satl.obj scsiport.lib ntoskrnl.lib
```

Replace `DRIVERPATH` with the path to `src\NT`. Output: `ahcint.sys`, targeting NT 3.50 and later (`-subsystem:native,3.50`).

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

`-c` forces a clean rebuild and `-Z` prints full build output to the console. Output: `obj\i386\ahcint.sys` (checked) or `obji386\ahcint.sys` (free), depending on the environment chosen. Use the **checked** environment for a debug build (asserts enabled, unoptimized), **free** for release.

#### For Windows XP x64 / Server 2003 x64 (amd64)

```bat
REM Set up build environment
Start Menu → Windows Server 2003 (WNET) DDK/WDK → Build Environments → x64 Free Build Environment
REM (runs setenv.bat ... WNET AMD64, configuring the amd64 cross-tools and libraries)

REM Navigate to driver directory
cd <path-to-AHCINT>\src\XPAMD64

REM Build
build -cZ
```

Output: `ahcint.sys` under the amd64 output directory (`objfre_wnet_amd64\amd64\` or equivalent, depending on the exact DDK/WDK version). `ahcint.rc` supplies version-resource information for this target.

> All three builds should first be verified in the **checked** build environment before being rebuilt and shipped from the **free** build environment.

## Installation

### Creating Installation Media

```
AHCINT\
├── ahcint.inf         (2000/XP and XP x64/Server 2003 targets)
├── i386\
│   └── ahcint.sys      (x86 binary)
└── amd64\
    └── ahcint.sys      (x64 binary)
```

NT 3.50/3.51/4.0 uses its own OEM Setup layout instead (`oemsetup.inf` + `txtsetup.oem` + `ahcint.sys`, see `bin\NT\floppy\`).

### Installing

#### GUI-mode setup (Windows 2000/XP/XP x64/Server 2003)

Use `bin\2KXP\ahcint.inf` (x86) or `bin\XPAMD64\ahcint.inf` (x64) with **"Have Disk"** during a manual driver install, or place the matching `.sys`/`.inf` pair where Plug and Play can find them. The driver installs as service `AHCINT` under `LoadOrderGroup = SCSI Miniport`.

**Note:** The driver expects the AHCI controller to be visible on the PCI bus with class code `01-06-01` (Mass Storage - SATA - AHCI).

#### Text-mode (F6) setup — Windows 2000/XP/XP x64/Server 2003

Copy the contents of the corresponding `bin\<target>\floppy\` folder onto a floppy disk (or a virtual floppy image for VM installs), press **F6** at the start of text-mode setup, and select **AHCINT SATA AHCI Storage Controller**. Required whenever the install disk itself sits behind the AHCI controller.

#### NT 3.50 / 3.51 / NT 4.0

NT uses the older OEM Setup mechanism (`oemsetup.inf`) rather than a standard `.inf`/`.cat` pair. Use **"Have Disk"** during setup (GUI-mode) or the equivalent F6 OEM prompt (text-mode) and point it at `bin\NT\floppy\`, which contains `oemsetup.inf`, `txtsetup.oem`, and `ahcint.sys`.

## Configuration

Registry values set at install time (see `ahcint.inf` / `txtsetup.oem` `[Config.*]` sections):

- **Tag** – boot-load ordering tag (differs per target/build)
- **Group** – `SCSI Miniport`
- **Type / Start / ErrorControl** – standard `SERVICE_KERNEL_DRIVER` / boot-start / normal-error-control service values

## Debugging

All three variants log via `DbgPrint`/`AHCI_DBG_MSG`/`AHCI_DBG_LOG`, visible through a kernel debugger (e.g. WinDbg / i386kd, depending on target OS) or `DebugView` once a debugger port is attached. Debug output is prefixed `[AHCINT]`.

## Technical Notes

### I/O Model

- **NT target:** `HwStartIo` issues the command and busy-waits on `PxCI` (bounded stall loop) before returning — a single command is always fully resolved before `HwStartIo` returns control to ScsiPort.
- **2000/XP and XP x64 targets:** `HwStartIo` builds the command and returns immediately (request left pending); completion is signaled from `HwInterrupt`, which checks the active SRB against the completing port and completes it on CI-clear or a fatal `TFD` error.

### Memory Model (XP x64 / Server 2003 x64)

- Pointer-difference bounds checks in DMA buffer allocation use `ULONG_PTR` rather than `ULONG`, since `ptrdiff_t` is 64-bit on x64 and would otherwise truncate.
- Relies on the amd64 WDK's own intrinsic-backed `KeMemoryBarrier()` rather than a hand-rolled x86 inline-asm barrier, used around shared-state (`ActiveSrb`) access in `HwResetBus` for SMP correctness.

### Memory Allocation

- **DMA arena** – 64 KB (`RawBuffer`), shared across all ports for command lists, FIS receive buffers, command tables, and cached IDENTIFY data
- **PRDT** – up to 32 entries per command, yielding an effective ~128 KB maximum transfer size per I/O

## License

Copyright (c) 2026 ages2001. All rights reserved.

**USE AT YOUR OWN RISK.** This driver talks directly to storage controller hardware on operating systems no longer supported or patched by Microsoft. Not recommended for any system holding data you can't afford to lose.

## Acknowledgments

- The AHCI 1.x and ATA/ATAPI-8 specifications
- Windows NT4/2000/XP DDK documentation

## Special Thanks

- infuscomus for porting driver to x64
- Dietmar for FAST_POLL idea
- DominBear for (nvme2k project)[https://github.com/techomancer/nvme2k]
- (UniATA project)[http://alter.org.ua/]
- Windows 2000 Dev Community
- Testers
- And everyone which supports it
