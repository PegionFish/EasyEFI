#!/usr/bin/env bash
# mktestimage 容器内执行体（由 mktestimage.sh 调用，不直接运行）。
set -euo pipefail
cd /work
case "$ARCH" in X64) BOOTNAME=BOOTX64.EFI;; AARCH64) BOOTNAME=BOOTAA64.EFI;; *) echo bad arch; exit 2;; esac
if [[ "$VARIANT" == fat ]]; then SIZE="${SIZE:-64}"; else SIZE="${SIZE:-192}"; fi
truncate -s "${SIZE}M" "$OUT"
sgdisk -Z "$OUT" >/dev/null
if [[ "$VARIANT" == fat ]]; then
  sgdisk -n 1:1M:0 -t 1:EF00 -c 1:TESTESP "$OUT" >/dev/null
else
  sgdisk -n 1:1M:+64M -t 1:EF00 -c 1:TESTESP -n 2:0:0 -t 2:0700 -c 2:TOOLKIT "$OUT" >/dev/null
fi
LOOP=$(losetup -fP --show "$OUT")
cleanup() { umount /mnt/p1 2>/dev/null || true; umount /mnt/p3 2>/dev/null || true; losetup -d "$LOOP" 2>/dev/null || true; }
trap cleanup EXIT
mkfs.vfat -F32 -n TESTESP "${LOOP}p1" >/dev/null
mkdir -p /mnt/p1
mount "${LOOP}p1" /mnt/p1
mkdir -p /mnt/p1/EFI/BOOT /mnt/p1/EFI/Apps /mnt/p1/EFI/Drivers
cp "$SHELL_EFI" "/mnt/p1/EFI/BOOT/$BOOTNAME"
FIRST_APP=""
while IFS= read -r entry; do
  [[ -z "$entry" ]] && continue
  name="${entry%%=*}"; path="${entry#*=}"
  cp "$path" "/mnt/p1/EFI/Apps/${name}.efi"
  [[ -z "$FIRST_APP" ]] && FIRST_APP="$name"
done <<< "$APPS"
if [[ "$VARIANT" == ntfs ]]; then
  [[ -n "$DRIVER" ]] || { echo "ntfs variant needs --driver"; exit 2; }
  cp "$DRIVER" /mnt/p1/EFI/Drivers/ntfs.efi
  mkfs.ntfs -f -L TOOLKIT "${LOOP}p2" >/dev/null
  mkdir -p /mnt/p3
  mount -t ntfs-3g "${LOOP}p2" /mnt/p3
  printf 'easyefi-test-image v0\n' > /mnt/p3/.easyefi
  if [[ -n "$DATADIR" && -d "$DATADIR" ]]; then cp -r "$DATADIR"/. /mnt/p3/; fi
  umount /mnt/p3
fi
if [[ -n "$STARTUP" ]]; then
  cp "$STARTUP" /mnt/p1/EFI/BOOT/startup.nsh
  cp "$STARTUP" /mnt/p1/startup.nsh
elif [[ -n "$FIRST_APP" ]]; then
  {
    echo "echo -off"
    if [[ "$VARIANT" == ntfs ]]; then
      echo "load EFI\\Drivers\\ntfs.efi"
      echo "map -r"
    fi
    echo "EFI\\Apps\\${FIRST_APP}.efi"
  } > /mnt/p1/EFI/BOOT/startup.nsh
  cp /mnt/p1/EFI/BOOT/startup.nsh /mnt/p1/startup.nsh
fi
umount /mnt/p1
echo "image ready: $OUT (${SIZE}M, $VARIANT, $ARCH)"
