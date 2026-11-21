#!/usr/bin/env bash
# S17: 原版 Shell 双架构构建固化（不魔改 ShellPkg；PCD 经 --pcd 覆盖）。
# 用法: bash scripts/build-shell.sh [X64|AARCH64|all]
# 环境: SHELL_STARTUP_DELAY（startup.nsh 倒计时秒数，默认 5）；EDK2_DIR 覆盖共享 edk2。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDK2="${EDK2_DIR:-$HOME/.cache/easyefi/edk2}"
DELAY="${SHELL_STARTUP_DELAY:-5}"
WHICH="${1:-all}"
arches=()
case "$WHICH" in X64|AARCH64) arches=("$WHICH");; all) arches=(X64 AARCH64);; *) echo "bad arch"; exit 2;; esac
for ARCH in "${arches[@]}"; do
  echo "=== Shell $ARCH (startup delay=${DELAY}s) ==="
  docker run --rm -e ARCH="$ARCH" -e DELAY="$DELAY" \
    -v "$ROOT":/work -v "$EDK2":/edk2:ro -w /work easyefi-build bash -c '
      set -e
      export WORKSPACE=/work PACKAGES_PATH=/edk2 EDK_TOOLS_PATH=/edk2/BaseTools CONF_PATH=/work/Conf
      mkdir -p /work/Conf
      source /edk2/edksetup.sh --reconfig >/dev/null 2>&1
      build -p ShellPkg/ShellPkg.dsc -m ShellPkg/Application/Shell/Shell.inf -a "$ARCH" -t GCC -b RELEASE \
        --pcd gEfiShellPkgTokenSpaceGuid.PcdShellDefaultDelay|"$DELAY"
    '
done
echo "产物: Build/Shell/RELEASE_GCC/<ARCH>/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi"
