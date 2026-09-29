# AHCINT - SATA AHCI Driver for Windows NT/2000/XP and Windows 95/98/Me

An AHCI (Advanced Host Controller Interface) SATA storage controller driver for Windows NT family and the Windows 95/98/Me, targeting operating systems from Windows 95 and Windows NT 3.50 up through Windows XP x64 Edition / Server 2003 x64 — long before Microsoft shipped a native AHCI driver.

## Overview

AHCINT is a SCSI miniport driver built on the classic `ScsiPort` framework (`SCSIPORT.PDR` on Windows 9x/Me, `scsiport.sys` on the NT family). It talks directly to AHCI HBA/port registers (command lists, PRDTs, FIS structures) and translates SCSI requests from the Windows storage stack into ATA/ATAPI commands, so SATA AHCI controllers can be used as boot and data devices on operating systems that predate AHCI entirely.

### Why?

Because plenty of real hardware only exposes its SATA ports in AHCI mode, and 9x/Me/NT/2000/XP have no idea what that is.

## Features

- **AHCI 1.0-class hardware support**
  - HBA generic registers (GHC, IS, PI, CAP, CAP2) and per-port registers (CLB, FB, IS, IE, CMD, TFD, SIG, SSTS, SCTL, SERR, CI)
  - Command List / Command Header / PRDT / FIS-based command submission
  - Port state machine handling: COMRESET via `SCTL.DET`, FRE/ST start-stop sequencing, Command List Override (CLO) fallback when a port won't idle
  - BIOS/OS Handoff Control (BOHC) so the driver takes ownership cleanly from the firmware

- **SCSI-to-ATA Translation Layer (SATL)**
  - INQUIRY, REQUEST SENSE, TEST UNIT READY, SYNCHRONIZE CACHE, VERIFY, MEDIUM REMOVAL, READ CAPACITY(10), MODE SENSE(6)
  - READ/WRITE(6), READ/WRITE(10) and READ/WRITE(16) on all targets
  - 64-bit SCSI on every target: READ CAPACITY(16) (SERVICE ACTION IN), READ/WRITE/VERIFY(16), SYNCHRONIZE CACHE(16), MODE SENSE(10) long LBA block descriptor (LLBAA); READ CAPACITY(10) returns `0xFFFFFFFF` for disks past 2 TB so the upper layer switches to READ CAPACITY(16)
  - ATA disks report INQUIRY VERSION `05h` (SPC-3 / SBC-2, as nvme9x does), advertising 16-byte CDB support; VPD pages `00h` and `80h` (serial number from IDENTIFY)
  - 28-bit READ/WRITE DMA whenever the request fits; READ/WRITE DMA EXT only on devices whose IDENTIFY DEVICE reports the 48-bit Address feature set (word 83, bit 10)
  - ATAPI (PACKET command) pass-through for CD/DVD-class devices, alongside plain ATA disk support
  - Real ATAPI media status: TEST UNIT READY / MEDIUM REMOVAL are passed through to the drive, REQUEST SENSE returns the drive's own sense data, and MODE SENSE(6) is translated to ATAPI MODE SENSE(10) and back — so media changes and empty trays are reported correctly to the CD-ROM class driver
  - IDENTIFY DEVICE / IDENTIFY PACKET DEVICE based device enumeration, with cached string extraction for INQUIRY vendor/product fields

- **Multi-target support from one source tree**
  - Windows 95 / 98 / Me (x86) — `SCSIPORT.PDR` miniport (`ahcint9x.mpd`)
  - Windows NT 3.50 / 3.51 / NT 4.0 (x86) — `ahcint.sys`
  - Windows 2000 / XP / Server 2003 (x86) and Windows XP x64 Edition / Server 2003 x64 (amd64) — `ahcint.sys`
  - Every target uses the same synchronous Fast Polling I/O path; the few real differences are `#ifdef`s (see [One source tree](#one-source-tree))

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

- **src\ahcimain.c** – Driver entry, PCI discovery (with bus-scan fallback), adapter/port initialization, `HwStartIo`, `HwInterrupt`, `HwResetBus`, DMA resource allocation; on 9X also `HwAdapterState`, the `_MapPhysToLinear` ABAR mapping and the VMM debug-print helper
- **src\ahcisatl.c** – SCSI command dispatch, SCSI↔ATA/ATAPI translation, 28/48-bit command selection, INQUIRY/READ CAPACITY/MODE SENSE synthesis, sense handling, PRDT construction, the Fast Polling command engine
- **src\ahcint.h** – Target switches (`AHCI_W9X`, `AHCI_NT4`), AHCI register layout, command/FIS structures, device extension, shared constants
- **src\9X\** – `build.bat`, NMAKE `makefile` (includes the DDK's `MASTER.MK`) and `ahcint9x.lnk` for Windows 9x/Me
- **src\NT\** – `build.bat` for NT 3.50/3.51/4.0
- **src\2KXP\** – `sources` / `makefile` / `ahcint.rc` for the DDK `build` utility (2000/XP/2003, x86 and amd64)
- **ahcint.inf** / **txtsetup.oem** / **oemsetup.inf** / **AHCINT9X.INF** – Installation files (in `bin\`) (see [Installation](#installation))

All targets compile the same three files in `src\`; the per-target folders only hold build files.

## Feature matrix by target

| | 95/98/Me (x86) | NT 3.50/3.51/4.0 (x86) | 2000/XP/2003 (x86) and XP x64/2003 x64 (amd64) |
|---|---|---|---|
| Build folder (sources are shared in `src\`) | `src\9X` | `src\NT` | `src\2KXP` |
| Target define | `AHCI_W9X` (implies `AHCI_NT4`) | `AHCI_NT4` | none |
| Driver binary | `ahcint9x.mpd` (`SCSIPORT.PDR` miniport) | `ahcint.sys` | `ahcint.sys` |
| I/O model | Synchronous Fast Polling — `HwStartIo` polls `PxCI`/`PxIS`/`PxTFD` every 10 µs (3 s timeout) | Same | Same |
| Interrupt usage | None — `PxIE` = 0 and `GHC.IE` off; `HwInterrupt` only acknowledges stray status | Same | Same |
| Max AHCI ports | 8 (build-time, see [Build-time limits](#build-time-limits)) | 8 (build-time) | 8 (build-time) |
| Max AHCI controllers | 8 (build-time, see [Build-time limits](#build-time-limits)) | 8 (build-time) | 8 (build-time) |
| PCI detection | Class code `01-06-01` plus known AMD/Intel device IDs; the bus:slot `SCSIPORT.PDR` hands in first, then a PCI bus-scan fallback | Same, bus:slot from ScsiPort first, then bus-scan fallback | Same, PnP-assigned bus:slot first, then bus-scan fallback |
| ABAR (MMIO) mapping | VMM `_MapPhysToLinear`, non-cached (KB Q169584 workaround) | `ScsiPortGetDeviceBase` | `ScsiPortGetDeviceBase` |
| ATA read/write commands | READ/WRITE DMA (28-bit) when the request ends below LBA 2^28 and moves ≤ 256 sectors; READ/WRITE DMA EXT only if IDENTIFY reports 48-bit support | Same | Same |
| READ/WRITE(16), READ CAPACITY(16), INQUIRY VERSION 05h (SPC-3) | Yes | Yes | Yes |
| Addressable capacity | 2^48 sectors (ATA limit) via READ CAPACITY(16); what the OS class driver can use is up to the OS | Same | Same |
| REQUEST SENSE | Yes — ATA errors (e.g. LBA out of range, UNC) are reported here; ATAPI asks the drive | Same | Yes |
| Auto-sense on error (`SRB_STATUS_AUTOSENSE_VALID`) | No — class driver issues REQUEST SENSE | No — class driver issues REQUEST SENSE | Yes — ATA errors mapped from `PxTFD`, ATAPI sense fetched from the drive |
| ATAPI (PACKET) support | Yes | Yes | Yes |
| ATAPI MODE SENSE(6)→(10) translation | Yes | Yes | Yes |
| 64-bit addressing (S64A) | No — 32-bit DMA only | No — 32-bit DMA only | Yes — 64-bit DMA when `CAP.S64A` is set (ABAR itself is read as a 32-bit BAR) |
| Hot-plug / late device detection | No | No | No |
| PRDT entries per command | 64 (33 at most in use: 32 physical breaks) | 64 | 64 |
| DMA arena size | 64 KB (shared across ports) | 64 KB (shared across ports) | 64 KB (shared across ports) |
| Max transfer size per I/O | ~128 KB | ~128 KB | ~128 KB |
| Debug output | VMM `_Debug_Printf_Service` (`DEBUG=1` builds), prefix `[ahcint9x]` | `DbgPrint`, prefix `[AHCINT]` | `DbgPrint`, prefix `[AHCINT]` |

## Known Limitations

- No NCQ (Native Command Queuing) — `PxSACT` is defined but never used; every port issues one command at a time via slot 0
- No interrupt-driven completion — every target completes commands by Fast Polling; no MSI/MSI-X support
- ~128 KB maximum transfer size per request (32 physical breaks), well short of AHCI's theoretical per-PRDT 4 MB limit
- No hot-plug support on any target; devices are detected once, at `HwInitialize`
- No power management (no device sleep/spin-down handling, no S3/S4 resume path beyond what ScsiPort provides)
- Single-queue-depth design — no per-port command queuing beyond the one in-flight command AHCINT itself tracks
- Disks past 2 TB need an upper layer that issues 16-byte CDBs (e.g. LBA64HLP.VXD on Windows 9x); the driver answers them on every target
- Only 512-byte logical sectors; CHS-only devices (no LBA) are not addressable (SATA requires LBA)
- SYNCHRONIZE CACHE and VERIFY are completed without issuing an ATA command
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
- **Windows NT 3.50 / 3.51 / NT 4.0 (x86):** Windows NT 4.0 DDK + Visual C++ 4.0 (MSVC 4.0)
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

`build.bat` sets `MASTER_MAKE=1` and the tool roots, puts MASM 6.11 and Visual C++ 2.0 on `PATH`, creates `TMP`/`TEMP` (default `C:\WINDOWS\TEMP`) if they are missing, warns if any expected tool is not found, and then runs `nmake` in `C:\AHCINT\9X`. The makefile pulls in the DDK's `MASTER.MK` (`BUILD_BITS=32`, `BUILD_TYPE=block`), compiles `..\ahcimain.c` and `..\ahcisatl.c` with `-DAHCI_W9X`, and links them with `ahcint9x.lnk`.

Output: `C:\AHCINT\9X\ahcint9x.mpd` (plus `ahcint9x.map`). Run `nmake clean` to remove build output.

#### For Windows NT 3.50 / 3.51 / NT 4.0 (x86)

```bat
cd <path-to-AHCINT>\src\NT
build.bat
```

`build.bat` sets `MSVCDIR` (default `C:\MSDEV`) and `DDKDIR` (default `C:\NT4DDK`) — edit them if your tools live elsewhere — then compiles `..\ahcimain.c` and `..\ahcisatl.c` with `-DAHCI_NT4` and links against `scsiport.lib` and `ntoskrnl.lib`.

Output: `src\NT\ahcint.sys`, targeting Windows NT 3.50 and later (`-subsystem:native,3.50`).

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

`sources` compiles the shared `..\ahcimain.c` and `..\ahcisatl.c` (no target define). `-c` forces a clean rebuild and `-Z` prints full build output to the console. Output: `obj\i386\ahcint.sys` (checked) or `obji386\ahcint.sys` (free), depending on the environment chosen. Use the **checked** environment for a debug build (asserts enabled, unoptimized), **free** for release.

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
    │   ├── ahcint.inf
    │   ├── ahcint.sys
    │   └── i386\ahcint.sys
    └── floppy_amd64\         (F6 disk, XP x64/2003 x64)
        ├── disk1             (tag file)
        ├── txtsetup.oem
        ├── ahcint.inf
        ├── ahcint.sys
        └── amd64\ahcint.sys
```

All three INF/OEM files match the controller by PCI class code (`PCI\CC_010601`, Mass Storage - SATA - AHCI).

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

#### Windows NT 3.50 / 3.51 / NT 4.0

NT uses the older OEM Setup mechanism (`oemsetup.inf`) rather than a standard `.inf`/`.cat` pair. Use **"Have Disk"** during setup (GUI-mode) or the equivalent F6 OEM prompt (text-mode), point it at `bin\NT\floppy\`, and select **AHCINT SATA AHCI Storage Controller (ages2001)**.

## Configuration

Registry values set at install time on the NT family (`ahcint.inf` service section, `txtsetup.oem` `[Config.scsi.AHCINT]`, `oemsetup.inf`):

- **Tag** – boot-load ordering tag: `40` on 2000/XP/x64, `33` on NT 3.50/3.51/4.0
- **Group** – `SCSI Miniport`
- **Type / Start / ErrorControl** – `1` (`SERVICE_KERNEL_DRIVER`) / `0` (boot start) / `1` (normal)
- **Event log** – `IoLogMsg.dll` registered as the event message file (`TypesSupported = 7`)
- **2000/XP/x64 only** – `Parameters\PnpInterface\5 = 1` (PnP on the PCI bus)

On Windows 9x/Me, `AHCINT9X.INF` registers the controller with `DevLoader = *IOS` and `PortDriver = AHCINT9X.MPD` (plus `DontLoadIfConflict = Y`); there are no extra settings to configure.

### Build-time limits

The port and controller limits are compile-time constants in `src\ahcint.h`. The defaults are 8 and 8; change them and rebuild if your hardware needs more.

```c
#define MAX_SUPPORTED_PORTS             8       /* AHCI ports per controller */
#define AHCI_MAX_CONTROLLERS            8       /* AHCI controllers per system */
```

**`MAX_SUPPORTED_PORTS`** is a port *number* limit, not a port count: ports are addressed by their index in the HBA's `PI` (Ports Implemented) register, and every port whose index is `MAX_SUPPORTED_PORTS` or higher is ignored, even if the controller has fewer ports in total. A chipset that implements only ports 0, 1, 8 and 9 needs a value of at least 10 to see all four. Each port becomes one SCSI target ID (port 0 = target 0, and so on). AHCI allows at most 32 ports, so the useful range is 1–32.

Raising it has these consequences, all in `src\ahcint.h` and `src\ahcimain.c`:

- **DMA arena** – every port takes 4 KB of the uncached DMA arena (command list 1 KB, received-FIS area 256 bytes, command table 2 KB, bounce buffer 512 bytes, plus alignment). The default `RawBuffer[65536]` in `AHCI_DMA_RESOURCES` holds 15 ports safely. For 16 or more ports, enlarge it to at least `(MAX_SUPPORTED_PORTS + 1) * 4096` bytes (the extra 4 KB covers alignment), for example `UCHAR RawBuffer[(MAX_SUPPORTED_PORTS + 1) * 4096];`. The driver does not check this at run time: too small an arena overruns the uncached extension.
- **Initiator ID** – ScsiPort reserves one target ID for the HBA itself and does not scan it. `AhciFindAdapter` sets `ConfigInfo->InitiatorBusId[0]` to `MAX_SUPPORTED_PORTS`, one past the last port, so it follows the limit automatically and never hides a port.
- **Device extension** – `HW_DEVICE_EXTENSION` grows by about 550 bytes per port (the `Ports[]` array, including a 512-byte copy of IDENTIFY data). This is ordinary non-paged memory and not a practical concern.
- **Uncached memory** – a larger arena means a larger physically contiguous `ScsiPortGetUncachedExtension` allocation per controller (128 KB + 4 KB for 32 ports). This is usually fine on NT and 2000/XP; on Windows 9x with little RAM a large contiguous allocation can fail, and `HwFindAdapter` then returns `SP_RETURN_ERROR`.
- **OS limits** – `ConfigInfo->MaximumNumberOfTargets` is set from `MAX_SUPPORTED_PORTS`. Values above 8 have not been tested on the 9x and NT 3.5x/4.0 targets; check that the OS's ScsiPort scans that many targets per bus.

**`AHCI_MAX_CONTROLLERS`** sizes the table that records which PCI bus:slot each driver instance has claimed, so that two instances never drive the same HBA. Each entry is 12 bytes; raising it costs nothing measurable. If there are more AHCI controllers than entries, the extra controllers are not recorded, and the PCI-scan fallback in `AhciFindAdapter` may then hand an already-driven HBA to a second instance, so keep this value at or above the number of AHCI controllers in the machine. A checked or `DEBUG=1` build logs `ClaimDevice: table full` when this happens.

After changing either value, rebuild every target you ship (`src\9X`, `src\NT`, `src\2KXP`); they all compile the same `ahcint.h`.

## Debugging

The NT, 2000/XP and x64 variants log via `DbgPrint` (through the `AHCI_DBG` macro), visible through a kernel debugger (e.g. WinDbg / i386kd, depending on target OS) or `DebugView` once a debugger port is attached. Debug output is prefixed `[AHCINT]`.

The 9X variant prints through the VMM's `_Debug_Printf_Service` (the same approach as SweetLow's nvme9x), so messages are fully formatted and go wherever Windows 9x debug output goes: WDEB386 on a serial port, SoftICE, or DebugView for Windows 9x. The calls are only compiled into `DEBUG=1` builds (`set DEBUG=1` before `build.bat`); release builds carry no debug code. Messages are prefixed `[ahcint9x]`.

In code, all targets use one macro with double parentheses (no variadic macros needed for VC++ 2.0 / MSVC 4.0):

```c
AHCI_DBG((AHCI_PFX "Port %lu: TFD=%08lX\n", port, tfd));
```

## Technical Notes

### I/O Model

- **All targets (Fast Polling):** every command, ATA or ATAPI, is issued from `HwStartIo` and polled every 10 µs on `PxCI`/`PxIS`/`PxTFD` (3 s timeout), then completed before `HwStartIo` returns. `PxIE` stays 0, so completion never depends on an interrupt being delivered — this also covers text-mode Setup, where the HAL has not assigned an IRQ. The loop exits as soon as `PxIS`/`PxTFD` report an error, so an empty CD tray answers NOT READY at once; the port is then stopped and restarted as the AHCI spec requires.

### ATA command selection

`AhciParseIdentify` reads IDENTIFY DEVICE once per disk. 48-bit addressing is used only when word 83 is valid (bits 15:14 = `01`), has bit 10 (48-bit Address feature set) set, and words 100–103 hold a capacity — the same rule as Linux's `ata_id_has_lba48()`. Capacity then comes from words 100–103, otherwise from words 60–61.

Each read/write is checked against that capacity (out of range → CHECK CONDITION, ILLEGAL REQUEST / LBA OUT OF RANGE) and sent as READ/WRITE DMA (28-bit) when it ends below LBA 2^28 and moves at most 256 sectors; only otherwise, and only on a 48-bit device, as READ/WRITE DMA EXT. A zero transfer length in READ/WRITE(10)/(16) completes without touching the disk (ATA would read a count of 0 as 256 / 65536 sectors).

LBAs are kept as two `ULONG`s (`AHCI_LBA`), so no `__int64` arithmetic — and none of the `_aulldiv`/`_allshl` CRT helpers the 9x build does not link — is needed.

### Windows 9x specifics

- **ABAR mapping:** `ScsiPortGetDeviceBase` can fail to return a usable linear address for a memory-mapped PCI BAR on Windows 95 (Microsoft KB Q169584). The 9X build maps the ABAR with the VMM's `_MapPhysToLinear` service instead, as non-cached (`MPL_NonCached`), so register polling always sees live hardware state.
- **`HwAdapterState`:** `SCSIPORT.PDR` calls this for PCI miniports during Plug and Play state changes; the 9X build provides it (a no-op that returns `TRUE`), because leaving it `NULL` makes `SCSIPORT.PDR` report "Init Failure" for the miniport.
- **PCI discovery:** like the NT targets, the bus:slot `SCSIPORT.PDR` hands in is tried first, then the PCI bus-scan fallback.

### One source tree

`src\ahcimain.c`, `src\ahcisatl.c` and `src\ahcint.h` build every target. The build files set the target:

| Define | Target | What it changes |
|---|---|---|
| `AHCI_W9X` | Windows 95/98/Me | ABAR mapped with VMM `_MapPhysToLinear` (KB Q169584); `HwAdapterState`; debug output via VMM `_Debug_Printf_Service`; implies `AHCI_NT4` |
| `AHCI_NT4` | NT 3.50/3.51/4.0 (and 9x) | No HBA reset (links taken over as the BIOS left them); ATAPI packets always carry the DMA bit; no `Dma64BitAddresses` in `PORT_CONFIGURATION_INFORMATION`; access range and INTx reported to ScsiPort by the miniport; no auto-sense (the class driver issues REQUEST SENSE) |
| none | 2000/XP/2003 x86 and x64 | HBA reset (`GHC.HR`) at init; 64-bit DMA when `CAP.S64A`; PnP-assigned interrupt; auto-sense |

The 2000/XP x86 and XP x64/Server 2003 x64 drivers differ only in the build environment; there are no architecture-specific code paths.

### Memory Allocation

- **DMA arena** – 64 KB (`RawBuffer`), shared across all ports for command lists, FIS receive buffers, command tables, and bounce buffers; 4 KB per port, enough for 15 ports (see [Build-time limits](#build-time-limits))
- **PRDT** – up to 64 entries per command; ScsiPort is told 32 physical breaks and 128 KB per I/O, so an unaligned 128 KB buffer (33 pages) still fits
- **Bounce buffer** – 512 bytes per port in the DMA arena, used for IDENTIFY and for the ATAPI REQUEST SENSE / MODE SENSE commands the driver issues itself

## License

Copyright (c) 2026 ages2001. All rights reserved.

**USE AT YOUR OWN RISK.** This driver talks directly to storage controller hardware on operating systems no longer supported or patched by Microsoft. Not recommended for any system holding data you can't afford to lose.

## Acknowledgments

- The AHCI 1.x and ATA/ATAPI-8 specifications
- Windows 95 DDK and Windows NT4/2000/XP DDK documentation

## Special Thanks

- infuscomus for porting driver to x64
- Dietmar for Fast Polling idea
- DominBear for [nvme2k project](https://github.com/techomancer/nvme2k)
- SweetLow for [nvme9x](https://github.com/LordOfMice/Tools) (Windows 9x VMM debug output and `_MapPhysToLinear` technique)
- [UniATA project](http://alter.org.ua/soft/win/uni_ata/)
- Windows 2000 Dev Community
- Testers
- And everyone which supports it
