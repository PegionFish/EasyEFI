#!/usr/bin/env bash
# S14 驱动冒烟（双架构）：mktestimage ntfs 变体 + qemu-run serial 断言。
# 用法: run-smoke.sh [x64|aa64|all]（默认 all）
# 前置: out/ntfs_*.efi 已构建（scripts/build-ntfs3g.sh）；
#       NtfsProbe 已构建（见 testapp/testapp.dsc 头注）；
#       主仓库 Shell.efi（S00/S17 产物，SHELL_BUILD 可覆盖）。
# 注意: mktestimage.sh 在容器内执行，--shell/--driver/--app/--out 一律传 /work 容器路径；
#       qemu-run.sh 在宿主执行，--image 传宿主路径。
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # third_party/ntfs-3g/smoke
NTFS="$(dirname "$DIR")"
ROOT="$(cd "$NTFS/../.." && pwd)"                     # worktree 根
SHELL_BUILD="${SHELL_BUILD:-/Users/bob/EasyEFI/Build/Shell/RELEASE_GCC}"
ARCH="${1:-all}"
case "$ARCH" in x64|aa64|all) ;; *) echo "usage: $0 [x64|aa64|all]" >&2; exit 2;; esac

cpath() { echo "/work${1#"$ROOT"}"; }   # worktree 宿主路径 → 容器路径

run_one() {
  local arch="$1" up driver shell probe img
  case "$arch" in
    x64)  up=X64;     driver="$NTFS/out/ntfs_x64.efi";;
    aa64) up=AARCH64; driver="$NTFS/out/ntfs_aa64.efi";;
  esac
  shell="$SHELL_BUILD/$up/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi"
  probe="$ROOT/Build/NtfsProbe/RELEASE_GCC/$up/third_party/ntfs-3g/testapp/NtfsProbe/OUTPUT/NtfsProbe.efi"
  [[ -f "$driver" ]] || { echo "missing driver: $driver (run scripts/build-ntfs3g.sh)" >&2; exit 1; }
  [[ -f "$shell" ]]  || { echo "missing Shell: $shell (S00/S17 产物)" >&2; exit 1; }
  [[ -f "$probe" ]]  || { echo "missing probe: $probe (见 testapp/testapp.dsc 头注构建命令)" >&2; exit 1; }

  # Shell.efi 拷入 worktree（mktestimage 容器只挂载 worktree）
  mkdir -p "$DIR/shell"
  cp "$shell" "$DIR/shell/Shell_$up.efi"

  img="$DIR/ntfs-$arch.img"
  echo "=== S14 smoke $arch: mktestimage (ntfs) ==="
  # 自带 startup.nsh：load 用 %homefilesystem% 绝对路径（design §3.3 形式）。
  # 实测 mktestimage 默认 startup 的裸相对路径 `load EFI\Drivers\ntfs.efi` 会
  # "File not found"（ShellOpenFileMetaArg 相对路径解析失败），见交接注记。
  bash "$ROOT/scripts/mktestimage.sh" --arch "$up" --variant ntfs \
    --out "$(cpath "$img")" \
    --shell "$(cpath "$DIR/shell/Shell_$up.efi")" \
    --driver "$(cpath "$driver")" \
    --app "NtfsProbe=$(cpath "$probe")" \
    --startup "$(cpath "$DIR/startup-ntfs.nsh")"
  echo "=== S14 smoke $arch: qemu-run ==="
  bash "$ROOT/scripts/qemu-run.sh" --arch "$arch" --image "$img" \
    --expect "$DIR/expect-ntfs-$arch.txt" --timeout 240
}

if [[ "$ARCH" == x64 || "$ARCH" == all ]]; then run_one x64; fi
if [[ "$ARCH" == aa64 || "$ARCH" == all ]]; then run_one aa64; fi
echo "S14 smoke: ALL PASS"