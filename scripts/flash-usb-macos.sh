#!/bin/bash
# flash-usb-macos.sh — EasyEFI 镜像写盘 + GPT 备份头迁移 + 分区级字节校验（macOS / 需 root）
# 用法: sudo /bin/bash flash-usb-macos.sh /dev/disk5
set -u
set -o pipefail

DEV="${1:-}"
[ "$DEV" = "/dev/disk5" ] || { echo "usage: $0 /dev/disk5"; exit 2; }
RAWDEV="/dev/rdisk5"
IMG="/Users/bob/EasyEFI/easyefi-integrated.img"
IMG_BYTES=4294967296
IMG_SHA=2aed1d10f69d3e5341709e6f8ff287e45c3f8c7939f3b0c01c293d5f9b416b31
LOG="/Users/bob/EasyEFI/.flash-usb.log"

log() { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LOG"; }
die() { log "FATAL: $*"; log "FLASH_RESULT=FAIL"; exit 1; }

: > "$LOG"
log "=== EasyEFI USB flash: dev=$DEV image=$IMG ==="

[ "$(id -u)" = "0" ] || die "must run as root (use sudo)"

info=$(diskutil info "$DEV" 2>/dev/null) || die "diskutil info $DEV failed"
printf '%s\n' "$info" | grep -qE 'Whole:.*Yes'          || die "target identity: not a whole disk"
printf '%s\n' "$info" | grep -qE 'Removable Media:.*Removable' || die "target identity: not removable"
printf '%s\n' "$info" | grep -qE 'Device Location:.*External'  || die "target identity: not external"
printf '%s\n' "$info" | grep -q 'USB' || die "target identity: not USB"
bytes=$(printf '%s\n' "$info" | sed -n 's/.*Disk Size:.*(\([0-9][0-9]*\) Bytes).*/\1/p')
[ -n "${bytes:-}" ] || die "cannot parse disk size"
[ "$bytes" -ge "$IMG_BYTES" ] || die "target too small: $bytes bytes"
log "OK: target $(printf '%s\n' "$info" | sed -n 's/^ *Disk Size: *//p') external USB ($DEV)"

[ -f "$IMG" ] || die "image missing: $IMG"
sz=$(stat -f %z "$IMG"); [ "$sz" = "$IMG_BYTES" ] || die "image size mismatch: $sz"
log "hashing image ($((IMG_BYTES/1073741824)) GiB)..."
got=$(shasum -a 256 "$IMG" | awk '{print $1}') || die "sha256 failed"
[ "$got" = "$IMG_SHA" ] || die "image sha256 mismatch: $got"
log "OK: image sha256: $got"

[ -c "$RAWDEV" ] || [ -b "$RAWDEV" ] || die "raw device $RAWDEV missing"

log "unmounting $DEV"
diskutil unmountDisk force "$DEV" >/dev/null 2>&1 || log "note: unmount reported failure (nothing mounted?)"

log "writing $IMG -> $RAWDEV (4 GiB, a few minutes)"
dd if="$IMG" of="$RAWDEV" bs=4m || die "dd failed"
sync
log "OK: dd complete"

sleep 1
diskutil unmountDisk force "$DEV" >/dev/null 2>&1 || true
log "relocating backup GPT to end of disk"
printf 'x\ne\nw\ny\n' | gdisk "$RAWDEV" >/tmp/gdisk-flash.out 2>&1 || die "gdisk relocate failed"
tail -n 4 /tmp/gdisk-flash.out | sed 's/^/  /' | tee -a "$LOG"
log "OK: gdisk relocate done"

diskutil unmountDisk force "$DEV" >/dev/null 2>&1 || true
gt=$(gdisk -l "$RAWDEV" 2>&1) || die "gdisk -l failed"
printf '%s\n' "$gt" | tee -a "$LOG" | sed -n '1,25p'
printf '%s\n' "$gt" | grep -qE 'last usable sector is 61439966' || die "backup GPT not at disk end"
printf '%s\n' "$gt" | grep -q 'TOOLKIT'  || die "TOOLKIT partition missing"
printf '%s\n' "$gt" | grep -q 'BOOTX64'  || die "BOOTX64 partition missing"
printf '%s\n' "$gt" | grep -q 'BOOTAA64' || die "BOOTAA64 partition missing"
log "OK: GPT verified (last usable 61439966; BOOTX64/BOOTAA64/TOOLKIT present)"

# --- 分区级字节校验：dd 后各分区内容必须与镜像逐字节一致 ---
# 跳过前 1 MiB（保护 MBR + 主 GPT），P1 起于 1 MiB、P2 起于 513 MiB、P3 起于 1025 MiB。
part_sha() {  # $1=文件或裸设备  $2=skip(MiB)  $3=count(MiB)
  dd if="$1" bs=1048576 skip="$2" count="$3" 2>/dev/null | shasum -a 256 | awk '{print $1}'
}

log "hashing image partitions (P1/P2/P3)..."
IMG_P1=$(part_sha "$IMG" 1 512)     || die "hash image P1 failed"
IMG_P2=$(part_sha "$IMG" 513 512)   || die "hash image P2 failed"
IMG_P3=$(part_sha "$IMG" 1025 3072) || die "hash image P3 failed"
log "OK: image partition sha256 P1=$IMG_P1 P2=$IMG_P2 P3=$IMG_P3"

for spec in 1:1:512:BOOTX64 2:513:512:BOOTAA64 3:1025:3072:TOOLKIT; do
  IFS=: read -r n skip cnt name <<< "$spec"
  case "$n" in 1) want="$IMG_P1" ;; 2) want="$IMG_P2" ;; 3) want="$IMG_P3" ;; esac
  log "verifying partition $n ($name) against image..."
  got=$(part_sha "$RAWDEV" "$skip" "$cnt") || die "hash device P$n failed"
  [ "$got" = "$want" ] || die "partition $n ($name) MISMATCH: got $got want $want"
  log "OK: partition $n ($name) matches image"
done

log "=== ALL DONE ==="
diskutil list "$DEV" | tee -a "$LOG"
log "FLASH_RESULT=OK"
