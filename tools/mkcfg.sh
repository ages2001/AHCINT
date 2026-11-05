#!/bin/sh
# mkcfg.sh [outdir]: the port programs (portpage.c with PP_EXE) of AHCINT, IDENT, NVMENT and
# MMCSDNT for Windows NT 3.1-4.0 and 95/98/Me (x86, runs on 2000/XP too); no C runtime
set -e
cd "$(dirname "$0")"; O=${1:-cfg}; mkdir -p $O
F="-Os -march=i386 -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -mno-stack-arg-probe -fno-asynchronous-unwind-tables -Wall -DPP_EXE"
for d in "PP_AHCI ahcicfg" "PP_IDE identcfg" "PP_NVME nvmecfg" "PP_MMC mmccfg"; do set -- $d
  i686-w64-mingw32-gcc $F -D$1 -Wl,--entry,_PpStart -Wl,--subsystem,windows:3.10 -Wl,--major-os-version,1 -Wl,--minor-os-version,0 \
    -Wl,--image-base,0x400000 -Wl,--disable-dynamicbase -Wl,--file-alignment,0x200 -Wl,--no-insert-timestamp \
    -o $O/$2.exe portpage.c -luser32 -ladvapi32 -lkernel32
done
# no DllCharacteristics (older loaders), image version 0
if command -v python3 >/dev/null && python3 -c "import pefile" 2>/dev/null; then
  python3 - $O <<'PY'
import pefile,glob,sys
for f in sorted(glob.glob(sys.argv[1]+'/*.exe')):
    p=pefile.PE(f); p.OPTIONAL_HEADER.DllCharacteristics=0; p.OPTIONAL_HEADER.MajorImageVersion=0; p.OPTIONAL_HEADER.CheckSum=p.generate_checksum(); d=p.write(); p.close(); open(f,'wb').write(d); print(f, len(d))
PY
fi
