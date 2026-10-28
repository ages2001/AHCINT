#!/bin/sh
# mkpp.sh [outdir]: the Device Manager page DLLs (portpage.c) of AHCINT, IDENT, NVMENT
# and MMCSDNT with MinGW-w64, x86 (outdir/i386) and x64 (outdir/amd64); no C runtime
set -e
cd "$(dirname "$0")"; O=${1:-pp}; mkdir -p $O/i386 $O/amd64
F="-Os -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -mno-stack-arg-probe -fno-asynchronous-unwind-tables -Wall -shared -DWINVER=0x0500 -D_WIN32_WINNT=0x0500 -D_WIN32_IE=0x0500"
L="-lsetupapi -lcomctl32 -luser32 -ladvapi32 -lkernel32"
for d in "PP_AHCI ahcipp" "PP_IDE identpp" "PP_NVME nvmentpp" "PP_MMC mmcsdpp"; do set -- $d
  i686-w64-mingw32-gcc $F -march=i486 -D$1 -Wl,--entry,_DllMainCRTStartup@12 -Wl,--kill-at -Wl,--subsystem,windows:5.0 \
    -Wl,--major-os-version,5 -Wl,--minor-os-version,0 -Wl,--disable-dynamicbase -Wl,--file-alignment,0x200 -Wl,--no-insert-timestamp -o $O/i386/$2.dll portpage.c $L
  x86_64-w64-mingw32-gcc $F -D$1 -Wl,--entry,DllMainCRTStartup -Wl,--subsystem,windows:5.2 \
    -Wl,--major-os-version,5 -Wl,--minor-os-version,2 -Wl,--disable-dynamicbase -Wl,--file-alignment,0x200 -Wl,--no-insert-timestamp -o $O/amd64/$2.dll portpage.c $L
done
# no DllCharacteristics (2000 and XP x64 do not know them), and the PE checksum
# (text-mode setup rejects a file listed in txtsetup.oem whose checksum is wrong)
if command -v python3 >/dev/null && python3 -c "import pefile" 2>/dev/null; then
  python3 - $O <<'PY'
import pefile,glob,sys
for f in sorted(glob.glob(sys.argv[1]+'/*/*.dll')):
    p=pefile.PE(f); p.OPTIONAL_HEADER.DllCharacteristics=0; p.OPTIONAL_HEADER.CheckSum=p.generate_checksum(); d=p.write(); p.close(); open(f,'wb').write(d); print(f, len(d))
PY
fi
