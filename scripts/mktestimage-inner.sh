#!/usr/bin/env bash
# mktestimage 容器内执行体（由 mktestimage.sh 调用，不直接运行）。
# 用偏移 loop（losetup -o）直接格式化分区区域——Docker Desktop VM 无 udev，
# loop 分区节点（loop0p1）不会出现，偏移 loop 绕开之。
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
# 解析分区起止扇区（512B）
readarray -t PARTS < <(sgdisk -p "$OUT" | awk '/^ +[0-9]+ +[0-9]+/ {print $2, $3}')
p1_start=${PARTS[0]%% *}; p1_end=${PARTS[0]##* }
P1_OFF=$((p1_start * 512)); P1_LEN=$(( (p1_end - p1_start + 1) * 512 ))
LOOP1=$(losetup -f --show -o "$P1_OFF" --sizelimit "$P1_LEN" "$OUT")
cleanup() { umount /mnt/p1 2>/dev/null || true; umount /mnt/p3 2>/dev/null || true;
            losetup -d "$LOOP1" 2>/dev/null || true; [[ -n "${LOOP2:-}" ]] && losetup -d "$LOOP2" 2>/dev/null || true; }
trap cleanup EXIT
mkfs.vfat -F32 -n TESTESP "$LOOP1" >/dev/null
mkdir -p /mnt/p1
mount "$LOOP1" /mnt/p1
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
  p2_start=${PARTS[1]%% *}; p2_end=${PARTS[1]##* }
  P2_OFF=$((p2_start * 512)); P2_LEN=$(( (p2_end - p2_start + 1) * 512 ))
  LOOP2=$(losetup -f --show -o "$P2_OFF" --sizelimit "$P2_LEN" "$OUT")
  mkfs.ntfs -f -L TOOLKIT "$LOOP2" >/dev/null
  mkdir -p /mnt/p3
  mount -t ntfs-3g "$LOOP2" /mnt/p3
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
