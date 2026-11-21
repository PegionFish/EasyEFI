#!/usr/bin/env bash
# 按模块构建（容器内，worktree 隔离）。
# 用法: build-module.sh <module.inf 相对仓库根> <X64|AARCH64> [额外 build 参数...]
# 例:  build-module.sh EasyEfiPkg/Application/Launcher/Launcher.inf X64
# 环境: EDK2_DIR 可覆盖共享 edk2 路径（默认 <repo>/edk2）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE="${1:?usage: build-module.sh <module.inf> <X64|AARCH64> [args]}"
ARCH="${2:?X64|AARCH64}"
shift 2
EDK2="${EDK2_DIR:-$ROOT/edk2}"
[[ -d "$EDK2" ]] || { echo "edk2 not found at $EDK2 (see docs/build-recipe.md)"; exit 1; }
docker run --rm \
  -e MODULE="$MODULE" -e ARCH="$ARCH" -e EXTRA="$*" -e DSC="${DSC:-EasyEfiPkg/EasyEfiPkg.dsc}" \
  -v "$ROOT":/work -v "$EDK2":/edk2:ro \
  -w /work easyefi-build bash -c '
    set -e
    export WORKSPACE=/work
    export PACKAGES_PATH=/edk2:/work
    export EDK_TOOLS_PATH=/edk2/BaseTools
    export CONF_PATH=/work/Conf
    source /edk2/edksetup.sh --reconfig >/dev/null 2>&1
    build -p "$DSC" -m "$MODULE" -a "$ARCH" -t GCC5 -b RELEASE $EXTRA
  '
