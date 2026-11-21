#!/usr/bin/env bash
# S16: 完整镜像流水线（design §3.1/§3.2/§8）。
# 用法: mkimage.sh --out <img> --staging <dir> [--size GB]
#   staging/P1、staging/P2、staging/P3 目录树分别拷入对应分区。
#   P1/P2 = FAT32 ESP 512MiB；P3 = NTFS（0700）剩余空间。
# 尺寸校验：staging/P3 内容总量 + 1.5GiB <= 镜像尺寸（design F06）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT=""; STAGING=""; SIZE_GB=8
while [[ $# -gt 0 ]]; do case "$1" in
  --out) OUT="$2"; shift 2;; --staging) STAGING="$2"; shift 2;; --size) SIZE_GB="$2"; shift 2;;
  *) echo "unknown $1" >&2; exit 2;; esac; done
[[ -n "$OUT" && -n "$STAGING" ]] || { echo "need --out --staging" >&2; exit 2; }
[[ -d "$STAGING/P1" && -d "$STAGING/P2" && -d "$STAGING/P3" ]] || { echo "staging needs P1/P2/P3 dirs" >&2; exit 2; }
docker run --rm --privileged \
  -e OUT="$OUT" -e STAGING="$STAGING" -e SIZE_GB="$SIZE_GB" \
  -v "$ROOT":/work -w /work easyefi-build bash /work/scripts/mkimage-inner.sh
