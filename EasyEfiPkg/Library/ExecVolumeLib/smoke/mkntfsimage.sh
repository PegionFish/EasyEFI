#!/usr/bin/env bash
# S09 ntfs 变体冒烟镜像（严格布局 design §3.1：P1 ESP x64 / P2 ESP aa64 / P3 NTFS TOOLKIT）。
# 不用 leader 的 mktestimage.sh ntfs 变体：其数据分区为 P2（非 P3），无法验证
# VolIsP3 的分区号判定；且其默认 startup 裸相对 load 路径有 S14 已报告 bug。
# 本脚本仅写 S09 目录产物，容器内偏移 loop 格式化（同 mktestimage-inner 配方）。
# 用法: mkntfsimage.sh --out <img> [--size MB]
# 依赖（worktree 内路径，可用 env 覆盖）：
#   SHELL_X64 / SHELL_AA64 / APP_X64 / APP_AA64 / NTFS_X64 / NTFS_AA64
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
OUT=""; SIZE=192
while [[ $# -gt 0 ]]; do case "$1" in
  --out) OUT="$2"; shift 2;; --size) SIZE="$2"; shift 2;;
  *) echo "unknown $1" >&2; exit 2;; esac; done
[[ -n "$OUT" ]] || { echo "need --out" >&2; exit 2; }
docker run --rm --privileged \
  -e OUT="$OUT" -e SIZE="$SIZE" \
  -e SHELL_X64="${SHELL_X64:-Build/smoke-s09/ShellX64.efi}" \
  -e SHELL_AA64="${SHELL_AA64:-Build/smoke-s09/ShellAA64.efi}" \
  -e APP_X64="${APP_X64:-Build/EasyEfiPkg/RELEASE_GCC/X64/SmokeS09.efi}" \
  -e APP_AA64="${APP_AA64:-Build/EasyEfiPkg/RELEASE_GCC/AARCH64/SmokeS09.efi}" \
  -e NTFS_X64="${NTFS_X64:-third_party/ntfs-3g/out/ntfs_x64.efi}" \
  -e NTFS_AA64="${NTFS_AA64:-third_party/ntfs-3g/out/ntfs_aa64.efi}" \
  -v "$ROOT":/work -w /work easyefi-build bash /work/EasyEfiPkg/Library/ExecVolumeLib/smoke/mkntfsimage-inner.sh
