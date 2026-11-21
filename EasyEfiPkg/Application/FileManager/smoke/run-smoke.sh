#!/usr/bin/env bash
# S12 冒烟编排（TEST-ONLY，我目录内；不修改 scripts/）。
# 用法: bash run-smoke.sh [aa64|x64|all] [fat|ntfs]   （默认 all ntfs）
# ntfs（任务书 DoD 变体）：进 P3 → 列目录 → 预览脚本 → 复制到 ESP → 执行 .nsh → 退出。
#   ⚠ 截至 2026-11-22 被 S14 阻塞：ntfs-3g 驱动目录句柄 Read 崩溃（uEnum 触发，
#   符号化证据见 docs/handoffs/S12.md）；S14 修复后直接复跑本模式。
# fat（interim 全链路实证）：双 USB 盘（ESP + 数据盘），导航/预览/跨卷复制/
#   执行确认/嵌套 Shell/退出 全链路与 ntfs 同构（仅文件系统不同）。
# 依赖: S00 Shell 产物（SHELL_X64/SHELL_AA64 可覆盖）、S14 ntfs 驱动（ntfs 模式）、
#       easyefi-build 容器、Homebrew QEMU。键盘由 qemu-run.sh 负责（勿再 --extra 加键盘）。
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
MODREL="EasyEfiPkg/Application/FileManager"
OUT="$HERE/build"
SHELL_X64="${SHELL_X64:-/Users/bob/EasyEFI/Build/Shell/RELEASE_GCC/X64/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi}"
SHELL_AA64="${SHELL_AA64:-/Users/bob/EasyEFI/Build/Shell/RELEASE_GCC/AARCH64/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi}"
mkdir -p "$OUT"
cd "$ROOT"

run_arch () {
  local arch="$1" variant="$2" ARCH low timeout srcshell driver app img img2 log starts exits fail=0
  local keys expect extra=""
  case "$arch" in
    x64)  ARCH=X64;     low=x64;  timeout=300; srcshell="$SHELL_X64";  driver="third_party/ntfs-3g/out/ntfs_x64.efi";;
    aa64) ARCH=AARCH64; low=aa64; timeout=180; srcshell="$SHELL_AA64"; driver="third_party/ntfs-3g/out/ntfs_aa64.efi";;
    *) echo "bad arch $arch" >&2; return 2;;
  esac
  [[ -f "$srcshell" ]] || { echo "Shell artifact missing: $srcshell" >&2; return 2; }

  echo "== [$arch/$variant] build FileManager =="
  bash scripts/build-module.sh "$MODREL/FileManager.inf" "$ARCH" || return 1
  app="Build/EasyEfiPkg/RELEASE_GCC/$ARCH/FileManager.efi"
  [[ -f "$app" ]] || { echo "app missing: $app" >&2; return 1; }
  cp "$srcshell" "$OUT/Shell-$arch.efi"

  if [[ "$variant" == ntfs ]]; then
    [[ -f "$ROOT/$driver" ]] || { echo "ntfs driver missing: $ROOT/$driver (S14)" >&2; return 2; }
    echo "== [$arch] make ntfs image (P3: scripts/fpt.nsh + readme.txt) =="
    bash scripts/mktestimage.sh --arch "$ARCH" --variant ntfs \
      --out "$MODREL/smoke/build/s12-$low.img" \
      --shell "$MODREL/smoke/build/Shell-$arch.efi" \
      --driver "$driver" \
      --app "FileManager=$app" \
      --data-dir "$MODREL/smoke/data" || return 1
    keys="$MODREL/smoke/keys-$low.txt"
    expect="$MODREL/smoke/expect-s12.txt"
  else
    echo "== [$arch] make fat images (boot ESP + data stick) =="
    bash scripts/mktestimage.sh --arch "$ARCH" --variant fat \
      --out "$MODREL/smoke/build/s12-fat-$low.img" \
      --shell "$MODREL/smoke/build/Shell-$arch.efi" \
      --app "FileManager=$app" \
      --extra-file "$MODREL/smoke/data-fat/test.nsh=test.nsh" || return 1
    bash scripts/mktestimage.sh --arch "$ARCH" --variant fat \
      --out "$MODREL/smoke/build/s12-fat-$low-stick2.img" \
      --shell "$MODREL/smoke/build/Shell-$arch.efi" || return 1
    keys="$MODREL/smoke/keys-fat-$low.txt"
    expect="$MODREL/smoke/expect-fat.txt"
    extra="-drive id=stick2,file=$OUT/s12-fat-$low-stick2.img,format=raw,if=none -device usb-storage,drive=stick2"
  fi
  img="$OUT/s12-$low.img"
  [[ "$variant" == fat ]] && img="$OUT/s12-fat-$low.img"

  echo "== [$arch/$variant] qemu run (key injection) =="
  bash scripts/qemu-run.sh --arch "$low" --image "$img" \
    --keys "$keys" --expect "$expect" --timeout "$timeout" --extra "$extra" || return 1
  log="${img%.img}.serial.log"

  echo "== [$arch/$variant] count assertions =="
  starts=$(grep -c '\[S12\] FileManager start' "$log" || true)
  exits=$(grep -c '\[S12\] FileManager exit' "$log" || true)
  if [[ "$starts" != 1 ]]; then
    echo "FAIL: FM start count=$starts (expect 1 → 嵌套 Shell 无 startup 递归)"; fail=1
  fi
  if [[ "$exits" != 1 ]]; then
    echo "FAIL: FM exit count=$exits (expect 1)"; fail=1
  fi
  if grep -q '\[S12\] FAIL' "$log"; then
    echo "FAIL: app reported FAIL markers:"; grep '\[S12\] FAIL' "$log"; fail=1
  fi
  if grep -q 'Synchronous Exception' "$log"; then
    echo "FAIL: Synchronous Exception in serial"; fail=1
  fi
  (( fail )) && return 1
  echo "PASS: [$arch/$variant] S12 smoke (expect + counts; serial: $log)"
}

WHICH="${1:-all}"
VARIANT="${2:-ntfs}"
rc=0
case "$WHICH" in
  x64)  run_arch x64 "$VARIANT"  || rc=1;;
  aa64) run_arch aa64 "$VARIANT" || rc=1;;
  all)  run_arch aa64 "$VARIANT" || rc=1; run_arch x64 "$VARIANT" || rc=1;;
  *) echo "usage: $0 [aa64|x64|all] [fat|ntfs]" >&2; exit 2;;
esac
exit $rc
