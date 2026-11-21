#!/usr/bin/env bash
# mkntfsimage.sh 容器内执行体（不直接运行）。严格三分区 + 双 ESP 同构 startup。
set -euo pipefail
cd /work
SIZE="${SIZE:-192}"
truncate -s "${SIZE}M" "$OUT"
sgdisk -Z "$OUT" >/dev/null
sgdisk -n 1:1M:+64M -t 1:EF00 -c 1:BOOTX64 \
       -n 2:0:+64M  -t 2:EF00 -c 2:BOOTAA64 \
       -n 3:0:0     -t 3:0700 -c 3:TOOLKIT "$OUT" >/dev/null
readarray -t PARTS < <(sgdisk -p "$OUT" | awk '/^ +[0-9]+ +[0-9]+/ {print $2, $3}')
LOOPS=()
cleanup() {
  local m
  for m in /mnt/s09p1 /mnt/s09p2 /mnt/s09p3; do umount "$m" 2>/dev/null || true; done
  local l
  for l in "${LOOPS[@]:-}"; do [[ -n "$l" ]] && losetup -d "$l" 2>/dev/null || true; done
}
trap cleanup EXIT
mkpart() {  # $1=分区序号(1-based) $2=挂载点；结果写入全局 LOOP（勿用 $() 调用，子壳会丢 LOOPS 记录）
  local idx=$(( $1 - 1 )) start end off len
  start=${PARTS[$idx]%% *}; end=${PARTS[$idx]##* }
  off=$(( start * 512 )); len=$(( (end - start + 1) * 512 ))
  LOOP=$(losetup -f --show -o "$off" --sizelimit "$len" "$OUT")
  LOOPS+=("$LOOP")
  mkdir -p "$2"
}
# startup.nsh：两 ESP 逐字节相同（design §3.3 形式；load 用 %homefilesystem% 绝对路径，S14 实证）
STARTUP=$(mktemp)
cat > "$STARTUP" <<'NSH'
echo -off
load %homefilesystem%\EFI\Drivers\ntfs.efi
map -r
%homefilesystem%\EFI\Apps\SmokeS09.efi
NSH
fill_esp() {  # $1=loop $2=挂载点 $3=BOOT名 $4=Shell $5=App $6=ntfs驱动
  mkfs.vfat -F32 -n "$3" "$1" >/dev/null
  mount "$1" "$2"
  mkdir -p "$2/EFI/BOOT" "$2/EFI/Apps" "$2/EFI/Drivers"
  cp "$4" "$2/EFI/BOOT/$3.EFI"
  cp "$5" "$2/EFI/Apps/SmokeS09.efi"
  cp "$6" "$2/EFI/Drivers/ntfs.efi"
  cp "$STARTUP" "$2/EFI/BOOT/startup.nsh"
  cp "$STARTUP" "$2/startup.nsh"
  umount "$2"
}
mkpart 1 /mnt/s09p1
fill_esp "$LOOP" /mnt/s09p1 BOOTX64 "$SHELL_X64" "$APP_X64" "$NTFS_X64"
mkpart 2 /mnt/s09p2
fill_esp "$LOOP" /mnt/s09p2 BOOTAA64 "$SHELL_AA64" "$APP_AA64" "$NTFS_AA64"
mkpart 3 /mnt/s09p3
mkfs.ntfs -f -L TOOLKIT "$LOOP" >/dev/null
mount -t ntfs-3g "$LOOP" /mnt/s09p3
printf 'easyefi-s09-smoke v0\n' > /mnt/s09p3/.easyefi
mkdir -p /mnt/s09p3/scripts
printf 'echo S09-P3-SCRIPT\n' > /mnt/s09p3/scripts/probe.nsh
umount /mnt/s09p3
rm -f "$STARTUP"
echo "ntfs image ready: $OUT (${SIZE}M, strict 3-partition layout)"
