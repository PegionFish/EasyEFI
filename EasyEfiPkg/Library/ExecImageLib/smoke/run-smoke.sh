#!/usr/bin/env bash
# S10 QEMU 冒烟编排（TEST-ONLY，我目录内；不修改 scripts/）。
# fat 变体 + 2 个 USB 镜像（启动盘 + 外部盘），双架构。
# 用法: bash run-smoke.sh [x64|aa64|all]   （默认 all；aa64 先行，HVF 快）
# 依赖: build/Shell-<arch>.efi（S00 Shell 产物的 worktree 内拷贝——mktestimage 容器
#       只挂载 worktree，宿主路径不可见；来源 /Users/bob/EasyEFI/Build/Shell/RELEASE_GCC/
#       <ARCH>/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi）、easyefi-build 容器、
#       Homebrew QEMU。
# 断言: expect-s10.txt 存在性（qemu-run）+ 计数（NSH 标记恰好 1 次=无递归；
#       目标恰好 1 次；auto 候选恰好 1=仅外部盘；无任何 FAIL 行）。
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
BUILD="$ROOT/Build/S10Smoke/RELEASE_GCC"
OUT="$HERE/build"
DSC="EasyEfiPkg/Library/ExecImageLib/smoke/Smoke.dsc"
mkdir -p "$OUT"

run_arch () {
  local arch="$1" ARCH qemu_arch shell app tgt bootimg extimg log
  local nsh target starts fail=0
  case "$arch" in
    x64)  ARCH=X64;     qemu_arch=x64;;
    aa64) ARCH=AARCH64; qemu_arch=aa64;;
    *) echo "bad arch $arch" >&2; return 2;;
  esac
  # mktestimage 容器只挂载 worktree（/work）→ Shell 用 worktree 内拷贝（相对 ROOT 路径）
  shell="EasyEfiPkg/Library/ExecImageLib/smoke/build/Shell-$arch.efi"
  if [[ ! -f "$ROOT/$shell" ]]; then
    echo "Shell artifact missing: $ROOT/$shell (从 S00 Build/Shell 拷贝，见脚本头注)" >&2
    return 2
  fi

  echo "== [$arch] build smoke apps (Smoke.dsc) =="
  DSC="$DSC" bash "$ROOT/scripts/build-module.sh" \
    EasyEfiPkg/Library/ExecImageLib/smoke/SmokeApp.inf "$ARCH" || return 1
  DSC="$DSC" bash "$ROOT/scripts/build-module.sh" \
    EasyEfiPkg/Library/ExecImageLib/smoke/SmokeTarget.inf "$ARCH" || return 1
  app=$(find "$BUILD/$ARCH" -maxdepth 1 -name "S10Smoke*.efi" | head -1)
  tgt=$(find "$BUILD/$ARCH" -maxdepth 1 -name "S10Target*.efi" | head -1)
  if [[ -z "$app" || -z "$tgt" ]]; then
    echo "smoke .efi artifacts not found under $BUILD/$ARCH" >&2
    return 1
  fi

  echo "== [$arch] make images (boot + external) =="
  bash "$ROOT/scripts/mktestimage.sh" --arch "$ARCH" --variant fat \
    --out "EasyEfiPkg/Library/ExecImageLib/smoke/build/boot-$arch.img" \
    --shell "$shell" \
    --app "S10Smoke=${app#"$ROOT"/}" \
    --app "S10Target=${tgt#"$ROOT"/}" || return 1
  bash "$ROOT/scripts/mktestimage.sh" --arch "$ARCH" --variant fat \
    --out "EasyEfiPkg/Library/ExecImageLib/smoke/build/ext-$arch.img" \
    --shell "$shell" || return 1
  bootimg="$OUT/boot-$arch.img"
  extimg="$OUT/ext-$arch.img"

  echo "== [$arch] qemu run (2 USB sticks) =="
  bash "$ROOT/scripts/qemu-run.sh" --arch "$qemu_arch" --image "$bootimg" \
    --expect "$HERE/expect-s10.txt" --timeout 300 \
    --extra "-drive id=stick2,file=$extimg,format=raw,if=none -device usb-storage,drive=stick2" \
    || return 1
  log="${bootimg%.img}.serial.log"

  echo "== [$arch] count assertions =="
  nsh=$(grep -c '\[S10-NSH\] marker' "$log" || true)
  target=$(grep -c '\[S10-TARGET\] ran' "$log" || true)
  starts=$(grep -c '\[S10\] smoke start' "$log" || true)
  if [[ "$nsh" != 1 ]]; then
    echo "FAIL: NSH marker count=$nsh (expect exactly 1 → no startup.nsh recursion)"; fail=1
  fi
  if [[ "$target" != 1 ]]; then
    echo "FAIL: target ran count=$target (expect 1)"; fail=1
  fi
  if [[ "$starts" != 1 ]]; then
    echo "FAIL: smoke app start count=$starts (expect 1)"; fail=1
  fi
  if ! grep -q '\[S10\] AUTO n=1 boot-excluded PASS' "$log"; then
    echo "FAIL: auto candidates != 1 (expect only the external disk)"; fail=1
  fi
  if grep -q 'FAIL' "$log"; then
    echo "FAIL: app reported FAIL markers:"; grep 'FAIL' "$log"; fail=1
  fi
  if (( fail )); then
    return 1
  fi
  echo "PASS: [$arch] S10 smoke (expect + counts)"
}

rc=0
case "${1:-all}" in
  x64)  run_arch x64  || rc=1;;
  aa64) run_arch aa64 || rc=1;;
  all)  run_arch aa64 || rc=1; run_arch x64 || rc=1;;
  *) echo "usage: run-smoke.sh [x64|aa64|all]" >&2; exit 2;;
esac
exit $rc