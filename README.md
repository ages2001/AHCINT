# AHCINT - SATA AHCI Driver for NT/2000/XP and Windows 95/98/Me

An AHCI (Advanced Host Controller Interface) SATA storage controller driver for Windows NT family and the Windows 95/98/Me, targeting operating systems from Windows 95 and Windows NT 3.50 up through Windows XP x64 Edition / Server 2003 x64 — long before Microsoft shipped a native AHCI driver.

## Overview

AHCINT is a SCSI miniport driver built on the classic `ScsiPort` framework (`SCSIPORT.PDR` on Windows 9x/Me, `scsiport.sys` on the NT family). It talks directly to AHCI HBA/port registers (command lists, PRDTs, FIS structures) and translates SCSI requests from the Windows storage stack into ATA/ATAPI commands, so SATA AHCI controllers can be used as boot and data devices on operating systems that predate AHCI entirely.

### Why?

Because plenty of real hardware only exposes its SATA ports in AHCI mode, and 9x/Me/NT/2000/XP have no idea what that is.

## Features

- **AHCI 1.0-class hardware support**
  - HBA generic registers (GHC, IS, PI, CAP, CAP2) and per-port registers (CLB, FB, IS, IE, CMD, TFD, SIG, SSTS, SCTL, SERR, CI)
  - Command List / Command Header / PRDT / FIS-based command submission
  - Port state machine handling: COMRESET via `SCTL.DET`, FRE/ST start-stop sequencing, Command List Override (CLO) fallback when a port won't idle (2000/XP target)
  - BIOS/OS Handoff Control (BOHC) so the driver takes ownership cleanly from the firmware

- **SCSI-to-ATA Translation Layer (SATL)**
  - INQUIRY, REQUEST SENSE, TEST UNIT READY, SYNCHRONIZE CACHE, VERIFY, MEDIUM REMOVAL, READ CAPACITY(10), MODE SENSE(6)
  - READ/WRITE(6) and READ/WRITE(10) on all targets; READ/WRITE(16) on the 2000/XP and XP x64 targets
  - ATAPI (PACKET command) pass-through for CD/DVD-class devices, alongside plain ATA disk support
  - Real ATAPI media status: TEST UNIT READY / MEDIUM REMOVAL are passed through to the drive, REQUEST SENSE returns the drive's own sense data, and MODE SENSE(6) is translated to ATAPI MODE SENSE(10) and back — so media changes and empty trays are reported correctly to the CD-ROM class driver
  - IDENTIFY DEVICE / IDENTIFY PACKET DEVICE based device enumeration, with cached string extraction for INQUIRY vendor/product fields

- **Multi-target support**
  - Windows 95 / 98 / Me (x86) — `SCSIPORT.PDR` miniport (`ahcint9x.mpd`), synchronous polling-based I/O
  - Windows NT 3.50 / 3.51 / NT 4.0 (x86) — synchronous polling-based I/O
  - Windows 2000 / XP / Server 2003 (x86) and Windows XP x64 Edition / Server 2003 x64 (amd64) — built from a single source tree, synchronous Fast Polling I/O

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

- **ahci_main.c** (`ahcimain.c` on 9X) – Driver entry, adapter/port initialization, `HwStartIo`, `HwInterrupt`, `HwResetBus`, DMA resource allocation (plus `HwAdapterState` and the `_MapPhysToLinear` ABAR mapping on 9X)
- **ahci_satl.c** (`ahcisatl.c` on 9X) – SCSI command dispatch, SCSI↔ATA/ATAPI translation, INQUIRY/MODE SENSE synthesis, PRDT construction
- **ahcint.h** (`ahcint9x.h` on 9X) – AHCI register layout, command/FIS structures, device extension, shared constants
- **build.bat** / **makefile** / **ahcint9x.lnk** – Windows 9x/Me build script, NMAKE makefile (includes the DDK's `MASTER.MK`) and linker response file
- **sources** / **makefile** / **ahcint.rc** – DDK `build` utility files for the 2000/XP and XP x64 targets
- **ahcint.inf** / **txtsetup.oem** / **oemsetup.inf** / **AHCINT9X.INF** – Installation files (in `bin\`) (see [Installation](#installation))

Source folders: `src\9X` (Windows 95/98/Me), `src\NT` (NT 3.50/3.51/4.0), `src\2KXP` (2000/XP/Server 2003, both x86 and amd64).

## Feature matrix by target

| | 95/98/Me (x86) | NT 3.50/3.51/4.0 (x86) | 2000/XP/2003 (x86) and XP x64/2003 x64 (amd64) |
|---|---|---|---|
| Source folder | `src\9X` | `src\NT` | `src\2KXP` |
| Driver binary | `ahcint9x.mpd` (`SCSIPORT.PDR` miniport) | `ahcint.sys` | `ahcint.sys` |
| I/O model | Synchronous — `HwStartIo` polls `PxCI`/`PxIS` every 20 µs (~0.5 s ATA / ~2 s ATAPI timeout) | Synchronous — same polling model as 9X | Synchronous Fast Polling — `HwStartIo` polls `PxCI`/`PxIS`/`PxTFD` every 10 µs (3 s timeout) |
| Interrupt usage | None — `PxIE` kept at 0, completion by polling | None — `PxIE` kept at 0, completion by polling | None in practice — `HwInterrupt` is registered (legacy INTx), but `PxIE` is cleared before every command |
| Max AHCI ports | 8 | 8 | 8 |
| Max AHCI controllers | 8 | 8 | 8 |
| PCI detection | Class code `01-06-01`, only on the bus:slot `SCSIPORT.PDR` hands in (no bus scan) | Class code `01-06-01`, with full-bus fallback scan | Class code `01-06-01` plus known AMD/Intel device IDs, with full-bus fallback scan |
| ABAR (MMIO) mapping | VMM `_MapPhysToLinear`, non-cached (KB Q169584 workaround) | `ScsiPortGetDeviceBase` | `ScsiPortGetDeviceBase` |
| ATA read/write commands | READ/WRITE DMA EXT (48-bit) | READ/WRITE DMA EXT (48-bit) | READ/WRITE DMA (28-bit) for small requests, READ/WRITE DMA EXT (48-bit) otherwise |
| READ/WRITE(16) | No — READ/WRITE(6)/(10) only | No — READ/WRITE(6)/(10) only | Yes |
| Addressable capacity | 2^32 sectors (~2 TB) | 2^32 sectors (~2 TB) | 2^32 sectors (~2 TB) — READ CAPACITY(10) only, capped at `0xFFFFFFFF` |
| REQUEST SENSE | Yes | Yes | Yes |
| Auto-sense on error (`SRB_STATUS_AUTOSENSE_VALID`) | No — class driver issues REQUEST SENSE | No — class driver issues REQUEST SENSE | Yes — filled from the `PxTFD` error byte |
| ATAPI (PACKET) support | Yes | Yes | Yes |
| ATAPI MODE SENSE(6)→(10) translation | Yes | Yes | Yes |
| 64-bit addressing (S64A) | No — 32-bit DMA only | No — 32-bit DMA only | Yes — 64-bit DMA when `CAP.S64A` is set (ABAR itself is read as a 32-bit BAR) |
| Hot-plug / late device detection | No | No | No |
| PRDT entries per command | 32 | 32 | 32 |
| DMA arena size | 64 KB (shared across ports) | 64 KB (shared across ports) | 64 KB (shared across ports) |
| Max transfer size per I/O | ~128 KB | ~128 KB | ~128 KB |
| Debug output | Raw COM1 UART (`0x3F8`), prefix `[ahcint9x]` | `DbgPrint`, prefix `[AHCINT]` | `DbgPrint`, prefix `[AHCINT]` |

## Known Limitations

- No NCQ (Native Command Queuing) — `PxSACT` is defined on the 2000/XP target but never used; every port issues one command at a time via slot 0
- No interrupt-driven completion — every target completes commands by polling (Fast Polling on 2000/XP/x64); no MSI/MSI-X support
- ~128 KB maximum transfer size per request (32 PRDT entries against a shared 64 KB DMA arena), well short of AHCI's theoretical per-PRDT 4 MB limit
- No hot-plug support on any target; devices are detected once, at `HwInitialize`
- No power management (no device sleep/spin-down handling, no S3/S4 resume path beyond what ScsiPort provides)
- Single-queue-depth design — no per-port command queuing beyond the one in-flight command AHCINT itself tracks
- Capacity is reported through READ CAPACITY(10) only (no READ CAPACITY(16)) on every target, so addressing tops out at 2^32 sectors (~2 TB at 512 bytes/sector). The 9X and NT builds issue LBA48 ATA commands (READ/WRITE DMA EXT) but only expose READ/WRITE(10) to SCSI; the 2000/XP/x64 build accepts READ/WRITE(16) but still reports at most 2^32 sectors. Disks larger than 2 TB are not supported
- 9X and NT builds lack READ/WRITE(16) and auto-sense; they are the least capable targets by design, matching the OS-era SCSI class driver's expectations
- 9X build only matches controllers by PCI class code `01-06-01`; controllers that report a different class code (e.g. RAID mode) are not picked up
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

`build.bat` expects the driver source in `C:\AHCINT9X` (it runs `cd \ahcint9x`), and `ahcint9x.lnk` links against `C:\DDK\BLOCK\LIB\scsiport.lib`. If your tools live elsewhere, edit the `SET` lines at the top of `build.bat` (`MASM_ROOT`, `C16_ROOT`, `C32_ROOT`, `SDKROOT`, `DDKROOT`) and the library path in `ahcint9x.lnk`.

```bat
REM Copy the 9X sources to C:\AHCINT9X
mkdir C:\AHCINT9X
copy <path-to-AHCINT>\src\9X\*.* C:\AHCINT9X

C:
cd \AHCINT9X

REM Optional: debug build (-DDEBLEVEL=1 -DDEBUG)
REM set DEBUG=1

REM Build
build.bat
```

`build.bat` sets `MASTER_MAKE=1` and the tool roots, puts MASM 6.11 and Visual C++ 2.0 on `PATH`, creates `TMP`/`TEMP` (default `C:\WINDOWS\TEMP`) if they are missing, warns if any expected tool is not found, and then runs `nmake`. The makefile pulls in the DDK's `MASTER.MK` (`BUILD_BITS=32`, `BUILD_TYPE=block`), compiles `ahcimain.c` and `ahcisatl.c`, and links them with `ahcint9x.lnk`.

Output: `C:\AHCINT9X\ahcint9x.mpd` (plus `ahcint9x.map`). Run `nmake clean` to remove build output.

#### For Windows NT 3.50 / 3.51 / NT 4.0 (x86)

```bat
cd <path-to-AHCINT>\src\NT

set MSVCDIR=C:\MSDEV
set DDKDIR=C:\NT4DDK

set PATH=%MSVCDIR%\BIN;%DDKDIR%\BIN;%PATH%
set INCLUDE=%DDKDIR%\inc;%DDKDIR%\src\storage\inc;%DDKDIR%\inc\crt;%MSVCDIR%\INCLUDE;C:\HYBRIDST\inc
set LIB=%DDKDIR%\lib\i386;%MSVCDIR%\LIB;C:\NT4DDK\lib\i386\free;%LIB%

del *.obj ahcint.sys
cl -nologo -c -Gz -Ox -W3 -Zp8 -Zi -D_X86_=1 -Di386=1 -DCONDITION_HANDLING=1 -DNT_UP=1 -DNT_INST=0 -DWIN32=100 -D_NT1X_=100 -DWINNT=1 -D_WIN32_WINNT=0x0350 /I..\inc ahci_main.c ahci_satl.c
link -nologo -debug -debugtype:both -subsystem:native,3.50 -entry:DriverEntry@8 -driver -base:0x10000 -align:0x200 -out:ahcint.sys ahci_main.obj ahci_satl.obj scsiport.lib ntoskrnl.lib
```

Output: `ahcint.sys`, targeting Windows NT 3.50 and later (`-subsystem:native,3.50`).

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

## Debugging

The NT, 2000/XP and x64 variants log via `DbgPrint`/`AHCI_DBG_MSG`/`AHCI_DBG_LOG`, visible through a kernel debugger (e.g. WinDbg / i386kd, depending on target OS) or `DebugView` once a debugger port is attached. Debug output is prefixed `[AHCINT]`.

The 9X variant writes its `AHCI_TRACE` checkpoints (prefixed `[ahcint9x]`) straight to the COM1 UART (I/O port `0x3F8`), with no debugger needed. Redirect the VM's COM1 to a file (e.g. QEMU `-serial file:com1.log`, or VirtualBox `--uart1 0x3F8 4 --uartmode1 file <path>`) to capture them. `DbgPrint` on 9X also goes out over COM1, but prints the literal format string (no `%` substitution, since no CRT is linked).

## Technical Notes

### I/O Model

- **9X and NT targets:** `HwStartIo` issues the command and polls `PxCI` and `PxIS` (20 µs stall per iteration, bounded) before returning — a single command is always fully resolved before `HwStartIo` returns control to ScsiPort. The loop also exits as soon as `PxIS` reports a task-file/fatal error, so an empty CD tray answers NOT READY at once instead of waiting out the timeout.
- **2000/XP and XP x64 targets (Fast Polling):** every command, ATA or ATAPI, is issued from `HwStartIo` and polled every 10 µs on `PxCI`/`PxIS`/`PxTFD` (3 s timeout), then completed before `HwStartIo` returns. `PxIE` is cleared before each command, so completion never depends on an interrupt being delivered — this also covers text-mode Setup, where the HAL has not assigned an IRQ. On an error, sense data is filled in straight from the `PxTFD` error byte.

### Windows 9x specifics

- **ABAR mapping:** `ScsiPortGetDeviceBase` can fail to return a usable linear address for a memory-mapped PCI BAR on Windows 95 (Microsoft KB Q169584). The 9X build maps the ABAR with the VMM's `_MapPhysToLinear` service instead, as non-cached (`MPL_NonCached`), so register polling always sees live hardware state.
- **`HwAdapterState`:** `SCSIPORT.PDR` calls this for PCI miniports during Plug and Play state changes; the 9X build provides it (a no-op that returns `TRUE`), because leaving it `NULL` makes `SCSIPORT.PDR` report "Init Failure" for the miniport.
- **No PCI bus scan:** `SCSIPORT.PDR` already enumerates PCI and calls `HwFindAdapter` once per PCI function, so the 9X build only checks the bus:slot it is given.

### x86 and x64 from one source tree

The 2000/XP x86 and XP x64/Server 2003 x64 drivers are built from the same `src\2KXP` sources; there are no architecture-specific code paths. 64-bit DMA addressing is enabled when the HBA reports `CAP.S64A`.

### Memory Allocation

- **DMA arena** – 64 KB (`RawBuffer`), shared across all ports for command lists, FIS receive buffers, command tables, and cached IDENTIFY data
- **PRDT** – up to 32 entries per command, yielding an effective ~128 KB maximum transfer size per I/O

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
- [UniATA project](http://alter.org.ua/soft//win/uni_ata/)
- Windows 2000 Dev Community
- Testers
- And everyone which supports it
