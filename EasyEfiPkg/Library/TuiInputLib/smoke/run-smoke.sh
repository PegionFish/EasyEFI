#!/usr/bin/env bash
# S07 冒烟编排：双架构构建测试应用 → fat 镜像 → QMP 按键注入 → serial 断言。
# 用法: bash smoke/run-smoke.sh [aa64|x64|all]   （默认 all；在模块目录或任意处运行）
# 依赖: S00 基建（build-module/mktestimage/qemu-run）、S00 Shell 产物（路径可经
#       SHELL_X64/SHELL_AA64 环境变量覆盖）、docker easyefi-build 镜像、原生 QEMU。
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
MODREL="EasyEfiPkg/Library/TuiInputLib"
WHICH="${1:-all}"
SHELL_X64="${SHELL_X64:-/Users/bob/EasyEFI/Build/Shell/RELEASE_GCC/X64/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi}"
SHELL_AA64="${SHELL_AA64:-/Users/bob/EasyEFI/Build/Shell/RELEASE_GCC/AARCH64/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi}"
cd "$ROOT"

run_arch () {
  local up="$1" low="$2" timeout="$3" shell_efi="$4" extra="$5"
  bash scripts/build-module.sh "$MODREL/Test/TuiInputTest.inf" "$up"
  mkdir -p Build/s07
  cp "$shell_efi" "Build/s07/Shell-$up.efi"
  bash scripts/mktestimage.sh --arch "$up" --variant fat --out "Build/s07/s07-$low.img" \
    --shell "Build/s07/Shell-$up.efi" \
    --app "TuiInputTest=Build/EasyEfiPkg/RELEASE_GCC/$up/TuiInputTest.efi"
  # aa64 virt 无默认键盘（QEMU 11 不自动加 usb-kbd，已用 info usb 实证）→ 显式挂 usb-kbd；
  # x64 q35 自带 ps2-kbd（唯一键盘，无双份输入风险），不加 usb-kbd 避免一键双事件。
  bash scripts/qemu-run.sh --arch "$low" --image "Build/s07/s07-$low.img" \
    --keys "$MODREL/smoke/keys-$low.txt" --expect "$MODREL/smoke/expect.txt" --timeout "$timeout" \
    --extra "$extra"
  echo "S07 smoke $low: PASS (serial: Build/s07/s07-$low.serial.log)"
}

case "$WHICH" in
  aa64) run_arch AARCH64 aa64 120 "$SHELL_AA64" "-device usb-kbd";;
  x64)  run_arch X64 x64 240 "$SHELL_X64" "";;
  all)  run_arch AARCH64 aa64 120 "$SHELL_AA64" "-device usb-kbd"; run_arch X64 x64 240 "$SHELL_X64" "";;
  *) echo "usage: $0 [aa64|x64|all]" >&2; exit 2;;
esac
