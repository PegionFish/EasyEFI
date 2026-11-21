#!/usr/bin/env bash
# S03 QEMU 冒烟编排：构建 app → fat 测试镜像 → mtools 注入 JSON 样例 → qemu-run 断言。
# 用法: bash run-smoke.sh [aa64|x64|both]   （默认 both；aa64 HVF 快，x64 TCG 慢）
# 依赖: easyefi-build 容器镜像、共享 edk2（~/.cache/easyefi/edk2）、Homebrew QEMU 固件、
#       主仓库 Build/Shell 的 Shell.efi（S00/S17 产物；可经 SHELL_EFI_X64/AA64 覆盖）。
# JSON 样例注入：mktestimage fat 变体不带数据文件 → 用 mtools 偏移 loop（P1 @1MiB）mcopy。
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
WHICH="${1:-both}"
SHELL_GUID=Shell_7C04A583-9E3E-4f1c-AD65-E05268D0B4D1.efi
SHELL_EFI_X64="${SHELL_EFI_X64:-/Users/bob/EasyEFI/Build/Shell/RELEASE_GCC/X64/$SHELL_GUID}"
SHELL_EFI_AA64="${SHELL_EFI_AA64:-/Users/bob/EasyEFI/Build/Shell/RELEASE_GCC/AARCH64/$SHELL_GUID}"
[[ -f "$HERE/expect-smoke.txt" ]] || python3 "$HERE/mkexpect.py" > "$HERE/expect-smoke.txt"

run_arch () {
  local ARCH="$1" UARCH SHELL_EFI IMG REL
  case "$ARCH" in
    aa64) UARCH=AARCH64; SHELL_EFI="$SHELL_EFI_AA64";;
    x64)  UARCH=X64;     SHELL_EFI="$SHELL_EFI_X64";;
    *) echo "bad arch $ARCH" >&2; exit 2;;
  esac
  IMG="$HERE/s03-smoke-$ARCH.img"
  REL="EasyEfiPkg/Library/ConfigCoreLib/TestApp/s03-smoke-$ARCH.img"
  echo "== S03 smoke $ARCH: build =="
  bash "$ROOT/scripts/build-module.sh" EasyEfiPkg/Library/ConfigCoreLib/TestApp/ConfigSmoke.inf "$UARCH"
  echo "== S03 smoke $ARCH: image =="
  # 容器仅挂载 worktree（/work）：--out/--shell/--app 一律 worktree 相对路径。
  # Shell.efi 来自主仓库 Build（S00/S17 产物），拷入本 worktree gitignored Build/。
  mkdir -p "$ROOT/Build"
  cp "$SHELL_EFI" "$ROOT/Build/Shell-$UARCH.efi"
  bash "$ROOT/scripts/mktestimage.sh" --arch "$UARCH" --variant fat --out "$REL" \
    --shell "Build/Shell-$UARCH.efi" \
    --app "ConfigSmoke=Build/EasyEfiPkg/RELEASE_GCC/$UARCH/ConfigSmoke.efi"
  echo "== S03 smoke $ARCH: inject JSON samples (mtools @1MiB) =="
  docker run --rm -v "$ROOT":/work easyefi-build bash -c "
    set -e
    mcopy -i /work/$REL@@1048576 /work/config/launcher.example.json ::/good.json
    mcopy -i /work/$REL@@1048576 /work/EasyEfiPkg/Library/ConfigCoreLib/TestApp/bad.json ::/bad.json
    mcopy -i /work/$REL@@1048576 /work/EasyEfiPkg/Library/ConfigCoreLib/TestApp/syntax.json ::/syntax.json
    mdir -i /work/$REL@@1048576 ::/ | tail -5"
  echo "== S03 smoke $ARCH: qemu =="
  local TIMEOUT=150; [[ "$ARCH" == x64 ]] && TIMEOUT=300
  bash "$ROOT/scripts/qemu-run.sh" --arch "$ARCH" --image "$IMG" \
    --expect "$HERE/expect-smoke.txt" --timeout "$TIMEOUT"
  echo "== S03 smoke $ARCH: PASS (serial: ${IMG%.img}.serial.log) =="
}

case "$WHICH" in
  aa64|x64) run_arch "$WHICH";;
  both) run_arch aa64; run_arch x64;;
  *) echo "usage: run-smoke.sh [aa64|x64|both]" >&2; exit 2;;
esac