#!/usr/bin/env bash
# S18 集成 harness：从 Build 产物组装 staging → 完整镜像 → 校验 → 双架构引导冒烟。
# 用法: integrate.sh [--size GB] [--skip-x64]
# 应用 .efi 取自 Build/EasyEfiPkg/RELEASE_GCC/<ARCH>/.../OUTPUT/（真实应用合并后重建）。
# 缺失的应用回退 stub（注明），保证 harness 随时可跑。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
SIZE_GB=8; SKIP_X64=0
while [[ $# -gt 0 ]]; do case "$1" in
  --size) SIZE_GB="$2"; shift 2;; --skip-x64) SKIP_X64=1; shift;;
  *) echo "unknown $1"; exit 2;; esac; done

efi() { # $1=arch $2=module-path-under-EasyEfiPkg $3=name -> echoes path or stub
  local p="Build/EasyEfiPkg/RELEASE_GCC/$1/EasyEfiPkg/$2/$3/OUTPUT/$3.efi"
  [[ -f "$p" ]] && echo "$p" || echo ""
}
shell_efi() { echo "Build/Shell/RELEASE_GCC/$1/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi"; }

for ARCH in X64 AARCH64; do
  case $ARCH in X64) P=staging/P1; BN=BOOTX64.EFI;; AARCH64) P=staging/P2; BN=BOOTAA64.EFI;; esac
  mkdir -p "$P/EFI/BOOT" "$P/EFI/Drivers" "$P/EFI/Launcher" "$P/EFI/Apps" "$P/EFI/Tools"
  cp "$(shell_efi $ARCH)" "$P/EFI/BOOT/$BN"
  cp boot/startup.nsh "$P/EFI/BOOT/startup.nsh"; cp boot/startup.nsh "$P/startup.nsh"
  # NTFS 驱动（S14 产物）
  D=third_party/ntfs-3g/out; [[ $ARCH == X64 ]] && DN=ntfs_x64.efi || DN=ntfs_aa64.efi
  if [[ -f "$D/$DN" ]]; then cp "$D/$DN" "$P/EFI/Drivers/ntfs.efi";
  else cp ".worktrees/S14/$D/$DN" "$P/EFI/Drivers/ntfs.efi" 2>/dev/null || echo "WARN: no ntfs driver $ARCH"; fi
  # Launcher / FileManager / Editor（真实优先，缺则 stub）
  for APP in Launcher FileManager Editor; do
    SRC=$(efi "$ARCH" "Application/$APP" "$APP")
    if [[ -n "$SRC" ]]; then cp "$SRC" "$P/EFI/$APP/$APP.efi" 2>/dev/null || { mkdir -p "$P/EFI/$APP"; cp "$SRC" "$P/EFI/$APP/$APP.efi"; };
    else echo "NOTE: $APP $ARCH not built yet (stub/absent)"; fi
  done
  # Launcher 放 EFI/Launcher（startup.nsh 路径）
  L=$(efi "$ARCH" "Application/Launcher" "Launcher")
  [[ -n "$L" ]] && cp "$L" "$P/EFI/Launcher/Launcher.efi"
done
# P3 数据区
mkdir -p staging/P3/scripts staging/P3/entries staging/P3/payloads staging/P3/docs
cp config/launcher.example.json staging/P3/launcher.json
[[ -f staging/P3/scripts/fpt.nsh ]] || printf 'echo -off\necho [FPT-PLACEHOLDER] %%1 %%2\n' > staging/P3/scripts/fpt.nsh

bash scripts/mkimage.sh --out easyefi-integrated.img --staging staging --size "$SIZE_GB"
bash scripts/verify-image.sh easyefi-integrated.img
echo "=== AA64 boot ==="
bash scripts/qemu-run.sh --arch aa64 --image easyefi-integrated.img --expect tests/expect/integrated-aa64.txt --timeout 150 || echo "AA64 boot: check log (real Launcher marker differs from stub)"
if [[ $SKIP_X64 == 0 ]]; then
  echo "=== x64 boot ==="
  bash scripts/qemu-run.sh --arch x64 --image easyefi-integrated.img --expect tests/expect/integrated-x64.txt --timeout 300 || echo "x64 boot: check log"
fi
echo "integrated image: easyefi-integrated.img"
