#!/usr/bin/env bash
# mkimage 容器内执行体（由 mkimage.sh 调用）。偏移 loop（Docker VM 无 loop 分区节点）。
set -euo pipefail
cd /work
# 尺寸校验（F06）：P3 内容 + 1.5GiB 余量
P3_KB=$(du -sk "$STAGING/P3" | cut -f1)
NEED_KB=$((P3_KB + 1572864))
HAVE_KB=$((SIZE_GB * 1048576))
if (( NEED_KB > HAVE_KB )); then
  echo "ERROR: image ${SIZE_GB}GiB too small for P3 content ($((P3_KB/1024))MiB) + 1.5GiB margin" >&2
  exit 1
fi
truncate -s "${SIZE_GB}G" "$OUT"
sgdisk -Z "$OUT" >/dev/null
sgdisk -n 1:1M:+512M -t 1:EF00 -c 1:BOOTX64 \
       -n 2:0:+512M  -t 2:EF00 -c 2:BOOTAA64 \
       -n 3:0:0      -t 3:0700 -c 3:TOOLKIT "$OUT" >/dev/null
readarray -t PARTS < <(sgdisk -p "$OUT" | awk '/^ +[0-9]+ +[0-9]+/ {print $2, $3}')
mkloop() { # $1=partition index(0-based) -> echoes loop dev
  local st=${PARTS[$1]%% *} en=${PARTS[$1]##* }
  losetup -f --show -o $((st*512)) --sizelimit $(( (en-st+1)*512 )) "$OUT"
}
L1=$(mkloop 0); L2=$(mkloop 1); L3=$(mkloop 2)
cleanup() { for m in /mnt/p1 /mnt/p2 /mnt/p3; do umount $m 2>/dev/null || true; done
            for l in "$L1" "$L2" "$L3"; do losetup -d "$l" 2>/dev/null || true; done; }
trap cleanup EXIT
mkfs.vfat -F32 -n BOOTX64  "$L1" >/dev/null
mkfs.vfat -F32 -n BOOTAA64 "$L2" >/dev/null
mkfs.ntfs -f -L TOOLKIT    "$L3" >/dev/null
mkdir -p /mnt/p1 /mnt/p2 /mnt/p3
mount "$L1" /mnt/p1; mount "$L2" /mnt/p2; mount -t ntfs-3g "$L3" /mnt/p3
cp -r "$STAGING/P1/." /mnt/p1/
cp -r "$STAGING/P2/." /mnt/p2/
cp -r "$STAGING/P3/." /mnt/p3/
# .easyefi 卷标记（P3 发现用，design §5.2）
if [[ ! -f /mnt/p3/.easyefi ]]; then
  printf 'EasyEFI image v0.1\nbuild %s\n' "$(date -u +%Y%m%dT%H%M%SZ)" > /mnt/p3/.easyefi
fi
sync
umount /mnt/p1; umount /mnt/p2; umount /mnt/p3
echo "image ready: $OUT (${SIZE_GB}GiB, strict 3-partition, P3=$((P3_KB/1024))MiB content)"
