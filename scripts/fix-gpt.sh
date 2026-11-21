#!/usr/bin/env bash
# S16: 写盘后 GPT 修复（design F08）。
# 用法: fix-gpt.sh <device|image> [--grow]
#   默认：sgdisk -e 迁移备份 GPT 到磁盘末尾（dd 到更大盘后必需）。
#   --grow：另扩容 P3 占满剩余空间（重建分区3 + ntfsresize）。
# 实盘操作前请确认设备路径（会修改分区表）。
set -euo pipefail
TARGET="${1:?usage: fix-gpt.sh <device|image> [--grow]}"
GROW="${2:-}"
IS_LOOP=0
if [[ -f "$TARGET" ]]; then
  TARGET_DEV=$(losetup -f --show "$TARGET"); IS_LOOP=1
else
  TARGET_DEV="$TARGET"
fi
cleanup() { [[ $IS_LOOP == 1 ]] && losetup -d "$TARGET_DEV" 2>/dev/null || true; }
trap cleanup EXIT
sgdisk -e "$TARGET_DEV" >/dev/null
echo "backup GPT relocated to disk end: $TARGET_DEV"
if [[ "$GROW" == "--grow" ]]; then
  START=$(sgdisk -p "$TARGET_DEV" | awk '/^ +3 +/ {print $2}')
  [[ -n "$START" ]] || { echo "no partition 3" >&2; exit 1; }
  sgdisk -d 3 -n 3:${START}:0 -t 3:0700 -c 3:TOOLKIT "$TARGET_DEV" >/dev/null
  END=$(sgdisk -p "$TARGET_DEV" | awk '/^ +3 +/ {print $3}')
  L3=$(losetup -f --show -o $((START*512)) --sizelimit $(( (END-START+1)*512 )) "$TARGET_DEV")
  ntfsresize -f "$L3" >/dev/null 2>&1 || { losetup -d "$L3"; echo "ntfsresize failed" >&2; exit 1; }
  losetup -d "$L3"
  echo "P3 grown to disk end"
fi
sgdisk -v "$TARGET_DEV" | tail -2
