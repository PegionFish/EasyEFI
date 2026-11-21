#!/usr/bin/env bash
# S13 Editor ntfs 冒烟（双架构）：打开 P3 中文脚本 → 插入标记 → 保存 → 退出 → 重开读回。
# 用法: run-smoke.sh [x64|aa64|all]（默认 all）
# 前置: Editor.efi 双架构已构建（scripts/build-module.sh EasyEfiPkg/Application/Editor/Editor.inf X64|AARCH64）；
#       ntfs 驱动 third_party/ntfs-3g/out/ntfs_*.efi（S14 产物，已入库）；
#       Shell.efi（S00/S17 产物，SHELL_BUILD 可覆盖）。
# 注意: mktestimage.sh 容器内执行，--shell/--driver/--app/--out/--data-dir/--startup 传 /work 容器路径；
#       qemu-run.sh 宿主执行，--image 传宿主路径。
# 卷编号: ntfs 测试镜像仅 P1(FAT)+P3(NTFS) 有文件系统（P2 未格式化）→ fs0=ESP, fs1=P3。
#       会话1 用 fs1: 全路径（glue 主路径：Shell GetDevicePathFromMap→DP 匹配）；
#       会话2 用相对路径（glue 兜底：各卷 Info 探测）。
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # EasyEfiPkg/Application/Editor/smoke
ROOT="$(cd "$DIR/../../../.." && pwd)"                 # worktree 根
SHELL_BUILD="${SHELL_BUILD:-/Users/bob/EasyEFI/Build/Shell/RELEASE_GCC}"
ARCH="${1:-all}"
case "$ARCH" in x64|aa64|all) ;; *) echo "usage: $0 [x64|aa64|all]" >&2; exit 2;; esac

cpath() { echo "/work${1#"$ROOT"}"; }

run_one() {
  local arch="$1" up driver shell editor img
  case "$arch" in
    x64)  up=X64;     driver="$ROOT/third_party/ntfs-3g/out/ntfs_x64.efi";;
    aa64) up=AARCH64; driver="$ROOT/third_party/ntfs-3g/out/ntfs_aa64.efi";;
  esac
  shell="$SHELL_BUILD/$up/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi"
  editor="$ROOT/Build/EasyEfiPkg/RELEASE_GCC/$up/EasyEfiPkg/Application/Editor/Editor/OUTPUT/Editor.efi"
  [[ -f "$driver" ]] || { echo "missing driver: $driver" >&2; exit 1; }
  [[ -f "$shell" ]]  || { echo "missing Shell: $shell (S00/S17 产物)" >&2; exit 1; }
  [[ -f "$editor" ]] || { echo "missing Editor: $editor (build-module.sh)" >&2; exit 1; }

  mkdir -p "$DIR/shell"
  cp "$shell" "$DIR/shell/Shell_$up.efi"

  img="$DIR/s13-$arch.img"
  # Docker Desktop virtiofs：复用已存在的 img 文件 losetup 会 ENOENT（S13 实证），先删旧。
  rm -f "$img"
  echo "=== S13 smoke $arch: mktestimage (ntfs) ==="
  bash "$ROOT/scripts/mktestimage.sh" --arch "$up" --variant ntfs \
    --out "$(cpath "$img")" \
    --shell "$(cpath "$DIR/shell/Shell_$up.efi")" \
    --driver "$(cpath "$driver")" \
    --app "Editor=$(cpath "$editor")" \
    --data-dir "$(cpath "$DIR/data")" \
    --startup "$(cpath "$DIR/startup-s13.nsh")"
  echo "=== S13 smoke $arch: qemu-run ==="
  bash "$ROOT/scripts/qemu-run.sh" --arch "$arch" --image "$img" \
    --keys "$DIR/keys-$arch.txt" --expect "$DIR/expect-s13.txt" --timeout 240
}

if [[ "$ARCH" == x64 || "$ARCH" == all ]]; then run_one x64; fi
if [[ "$ARCH" == aa64 || "$ARCH" == all ]]; then run_one aa64; fi
echo "S13 smoke: ALL PASS"
