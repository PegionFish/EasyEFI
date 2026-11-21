#!/usr/bin/env bash
# 最小测试镜像生成（容器内执行，--privileged：losetup/mount）。
# 用法: mktestimage.sh --arch X64|AARCH64 --variant fat|ntfs --out <img>
#        --shell <Shell.efi> [--app <name>=<efi> ...] [--driver <ntfs.efi>]
#        [--startup <nsh>] [--size MB] [--data-dir <dir>]
# fat 变体: 单 ESP(FAT32) + Shell + apps；ntfs 变体: ESP + NTFS P3 + 驱动。
# 默认 startup.nsh 自动运行第一个 app（ntfs 变体先 load 驱动 + map -r）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH=""; VARIANT=fat; OUT=""; SHELL_EFI=""; APPS=""; DRIVER=""; STARTUP=""; SIZE=""; DATADIR=""; EXTRAFILES=""
while [[ $# -gt 0 ]]; do case "$1" in
  --arch) ARCH="$2"; shift 2;; --variant) VARIANT="$2"; shift 2;; --out) OUT="$2"; shift 2;;
  --shell) SHELL_EFI="$2"; shift 2;; --app) APPS="$APPS"$'\n'"$2"; shift 2;;
  --driver) DRIVER="$2"; shift 2;; --startup) STARTUP="$2"; shift 2;;
  --size) SIZE="$2"; shift 2;; --data-dir) DATADIR="$2"; shift 2;;
  --extra-file) EXTRAFILES="$EXTRAFILES"$'\n'"$2"; shift 2;;
  *) echo "unknown $1" >&2; exit 2;; esac; done
[[ -n "$ARCH" && -n "$OUT" && -n "$SHELL_EFI" ]] || { echo "need --arch --out --shell" >&2; exit 2; }
EDK2="${EDK2_DIR:-$ROOT/edk2}"
docker run --rm --privileged \
  -e ARCH="$ARCH" -e VARIANT="$VARIANT" -e OUT="$OUT" -e SHELL_EFI="$SHELL_EFI" \
  -e APPS="$APPS" -e DRIVER="$DRIVER" -e STARTUP="$STARTUP" -e SIZE="$SIZE" -e DATADIR="$DATADIR" \
  -e EXTRAFILES="$EXTRAFILES" \
  -v "$ROOT":/work -w /work easyefi-build bash /work/scripts/mktestimage-inner.sh
