#!/usr/bin/env bash
# S11 Launcher QEMU 冒烟编排（fat + ntfs + degraded 三场景 × 双架构）。
# 用法: bash smoke/run-smoke.sh [aa64|x64|both]   （默认 both；aa64 HVF 快，x64 TCG 慢）
# 依赖: easyefi-build 容器、共享 edk2（~/.cache/easyefi/edk2）、Homebrew QEMU 固件、
#       主仓库 Build/Shell 的 Shell.efi（S00/S17 产物；SHELL_EFI_X64/AA64 可覆盖）、
#       third_party/ntfs-3g/out/ntfs_*.efi（S14 产物，已入库）。
# 执行目标: FileManager S00 stub（打印 [STUB] FileManager <arch>）——fat 场景经内置菜单
#           ExecEfi(fs0:/EFI/Apps/FileManager.efi)；ntfs 场景复制为 P3 entries/target.efi。
# 前置: Launcher 真实链接需 S03/S06/S09/S10 合并（S03 已合；其余见交接注记）。
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
WHICH="${1:-both}"
SHELL_GUID=Shell_7C04A583-9E3E-4f1c-AD65-E05268D0B4D1.efi
SHELL_EFI_X64="${SHELL_EFI_X64:-/Users/bob/EasyEFI/Build/Shell/RELEASE_GCC/X64/$SHELL_GUID}"
SHELL_EFI_AA64="${SHELL_EFI_AA64:-/Users/bob/EasyEFI/Build/Shell/RELEASE_GCC/AARCH64/$SHELL_GUID}"

run_arch () {
  local ARCH="$1" UARCH SHELL_EFI NTFS TAG
  case "$ARCH" in
    aa64) UARCH=AARCH64; SHELL_EFI="$SHELL_EFI_AA64"; NTFS="third_party/ntfs-3g/out/ntfs_aa64.efi"; TAG=aarch64;;
    x64)  UARCH=X64;     SHELL_EFI="$SHELL_EFI_X64";  NTFS="third_party/ntfs-3g/out/ntfs_x64.efi";  TAG=x64;;
    *) echo "bad arch $ARCH" >&2; exit 2;;
  esac
  local OUTDIR="Build/EasyEfiPkg/RELEASE_GCC/$UARCH/EasyEfiPkg/Application"
  local LAUNCHER="$OUTDIR/Launcher/Launcher/OUTPUT/Launcher.efi"
  local FM="$OUTDIR/FileManager/FileManager/OUTPUT/FileManager.efi"
  local DATA="Build/s11-data-$ARCH"
  local TIMEOUT=180; [[ "$ARCH" == x64 ]] && TIMEOUT=300

  echo "== S11 smoke $ARCH: build Launcher + FileManager stub =="
  bash "$ROOT/scripts/build-module.sh" EasyEfiPkg/Application/Launcher/Launcher.inf "$UARCH"
  bash "$ROOT/scripts/build-module.sh" EasyEfiPkg/Application/FileManager/FileManager.inf "$UARCH"

  echo "== S11 smoke $ARCH: stage Shell.efi + P3 data =="
  mkdir -p "$ROOT/Build" "$ROOT/$DATA/entries"
  cp "$SHELL_EFI" "$ROOT/Build/Shell-$UARCH.efi"
  cp -r "$HERE/data/." "$ROOT/$DATA/"
  cp "$ROOT/$FM" "$ROOT/$DATA/entries/target.efi"

  echo "== S11 smoke $ARCH: fat image (DEGRADED 内置菜单 + FileManager exec + ESC→Shell) =="
  # 自定义 startup：Launcher 返回后 echo 收尾标记——证明干净退出（捕获退出清理期崩溃；
  # 裸 "Shell>" 断言会误匹配启动初期提示符，故以收尾标记为准）。
  {
    echo "echo -off"
    echo "EFI\\Apps\\Launcher.efi"
    echo "echo [S11-EXIT-$TAG] launcher-returned"
  } > "$ROOT/Build/s11-startup-fat-$ARCH.nsh"
  bash "$ROOT/scripts/mktestimage.sh" --arch "$UARCH" --variant fat \
    --out "Build/s11-fat-$ARCH.img" --shell "Build/Shell-$UARCH.efi" \
    --startup "Build/s11-startup-fat-$ARCH.nsh" \
    --app "Launcher=$LAUNCHER" --app "FileManager=$FM"
  bash "$ROOT/scripts/qemu-run.sh" --arch "$ARCH" --image "$ROOT/Build/s11-fat-$ARCH.img" \
    --keys "$HERE/keys-fat.txt" --expect "$HERE/expect-fat-$ARCH.txt" --timeout "$TIMEOUT"

  echo "== S11 smoke $ARCH: ntfs image (P3:OK + exec + nsh + danger 确认 + ESC→Shell) =="
  {
    echo "echo -off"
    echo "load %homefilesystem%\\EFI\\Drivers\\ntfs.efi"
    echo "map -r"
    echo "EFI\\Apps\\Launcher.efi"
    echo "echo [S11-EXIT-$TAG] launcher-returned"
  } > "$ROOT/Build/s11-startup-ntfs-$ARCH.nsh"
  bash "$ROOT/scripts/mktestimage.sh" --arch "$UARCH" --variant ntfs \
    --out "Build/s11-ntfs-$ARCH.img" --shell "Build/Shell-$UARCH.efi" \
    --driver "$NTFS" --data-dir "$DATA" \
    --startup "Build/s11-startup-ntfs-$ARCH.nsh" \
    --app "Launcher=$LAUNCHER" --app "FileManager=$FM"
  bash "$ROOT/scripts/qemu-run.sh" --arch "$ARCH" --image "$ROOT/Build/s11-ntfs-$ARCH.img" \
    --keys "$HERE/keys-ntfs.txt" --expect "$HERE/expect-ntfs-$ARCH.txt" --timeout "$TIMEOUT"

  echo "== S11 smoke $ARCH: degraded image (假驱动 load 失败 → P3 缺失 → DEGRADED) =="
  {
    echo "echo -off"
    echo "load %homefilesystem%\\EFI\\Drivers\\ntfs.efi"
    echo "map -r"
    echo "EFI\\Apps\\Launcher.efi"
    echo "echo [S11-EXIT-$TAG] launcher-returned"
  } > "$ROOT/Build/s11-startup-degraded-$ARCH.nsh"
  bash "$ROOT/scripts/mktestimage.sh" --arch "$UARCH" --variant ntfs \
    --out "Build/s11-degraded-$ARCH.img" --shell "Build/Shell-$UARCH.efi" \
    --driver "EasyEfiPkg/Application/Launcher/smoke/fake-driver.bin" --data-dir "$DATA" \
    --startup "Build/s11-startup-degraded-$ARCH.nsh" \
    --app "Launcher=$LAUNCHER" --app "FileManager=$FM"
  bash "$ROOT/scripts/qemu-run.sh" --arch "$ARCH" --image "$ROOT/Build/s11-degraded-$ARCH.img" \
    --keys "$HERE/keys-degraded.txt" --expect "$HERE/expect-degraded.txt" --timeout "$TIMEOUT"

  echo "== S11 smoke $ARCH: ALL PASS (serial logs: Build/s11-*-$ARCH.serial.log) =="
}

case "$WHICH" in
  aa64|x64) run_arch "$WHICH";;
  both) run_arch aa64; run_arch x64;;
  *) echo "usage: run-smoke.sh [aa64|x64|both]" >&2; exit 2;;
esac
