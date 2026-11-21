#!/usr/bin/env bash
# 按模块构建（容器内，worktree 隔离）。
# 用法: build-module.sh <module.inf 相对仓库根> <X64|AARCH64> [额外 build 参数...]
# 例:  build-module.sh EasyEfiPkg/Application/Launcher/Launcher.inf X64
# 环境: EDK2_DIR 可覆盖共享 edk2 路径（默认 ~/.cache/easyefi/edk2；必须在仓库树外，
#       否则 WORKSPACE 与 PACKAGES_PATH 重复发现包触发 case-mismatch 检查）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE="${1:?usage: build-module.sh <module.inf> <X64|AARCH64> [args]}"
ARCH="${2:?X64|AARCH64}"
shift 2
EDK2="${EDK2_DIR:-$HOME/.cache/easyefi/edk2}"
[[ -d "$EDK2" ]] || { echo "edk2 not found at $EDK2 (see docs/build-recipe.md)"; exit 1; }
docker run --rm \
  -e MODULE="$MODULE" -e ARCH="$ARCH" -e EXTRA="$*" -e DSC="${DSC:-EasyEfiPkg/EasyEfiPkg.dsc}" \
  -v "$ROOT":/work -v "$EDK2":/edk2:ro \
  -w /work easyefi-build bash -c '
    set -e
    export WORKSPACE=/work
    export PACKAGES_PATH=/edk2
    export EDK_TOOLS_PATH=/edk2/BaseTools
    export CONF_PATH=/work/Conf
    mkdir -p /work/Conf
    source /edk2/edksetup.sh --reconfig >/dev/null 2>&1
    if grep -q "LIBRARY_CLASS" "/work/$MODULE"; then
      build -p "$MODULE" -a "$ARCH" -t GCC -b RELEASE $EXTRA
    else
      build -p "$DSC" -m "$MODULE" -a "$ARCH" -t GCC -b RELEASE $EXTRA
    fi
  '
