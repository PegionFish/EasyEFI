#!/usr/bin/env bash
# S14: ntfs-3g UEFI 读写驱动双架构构建（可复现）。
# 用法: build-ntfs3g.sh [X64|AARCH64|all]（默认 all）
# 前置: third_party/ntfs-3g/src/ 已由 fetch.sh 拉取锁定 commit（缺失时自动调用）。
# 环境: EDK2_DIR 可覆盖共享 edk2（默认 ~/.cache/easyefi/edk2，必须在仓库树外）。
# 产物: third_party/ntfs-3g/out/ntfs_x64.efi / ntfs_aa64.efi + SHA256SUMS。
# 配方依据 docs/build-recipe.md：-t GCC（非 GCC5）；CONF_PATH 显式预创建；
# WORKSPACE=驱动源码目录（uefi-driver.dsc 在其根），PACKAGES_PATH=/edk2。
# 上游 uefi-driver.dsc 已含 MdeLibs.dsc.inc + AARCH64 CompilerIntrinsicsLib NULL 链接。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NTFS="$ROOT/third_party/ntfs-3g"
ARCH="${1:-all}"
EDK2="${EDK2_DIR:-$HOME/.cache/easyefi/edk2}"
[[ -d "$EDK2" ]] || { echo "edk2 not found at $EDK2 (see docs/build-recipe.md)" >&2; exit 1; }
case "$ARCH" in X64|AARCH64|all) ;; *) echo "usage: $0 [X64|AARCH64|all]" >&2; exit 2;; esac

[[ -f "$NTFS/src/uefi-driver.dsc" ]] || bash "$NTFS/fetch.sh"
mkdir -p "$NTFS/out" "$NTFS/conf"

build_one() {
  local arch="$1"
  echo "=== ntfs-3g $arch RELEASE (GCC) ==="
  docker run --rm \
    -v "$ROOT":/work -v "$EDK2":/edk2:ro \
    -w /work/third_party/ntfs-3g/src easyefi-build bash -c '
      set -e
      # ntfs-3g 的 types.h 直接 #include <stdint.h>：X64 交叉编译需要目标架构 libc 头。
      # 基座镜像 --no-install-recommends 未装 libc6-dev-amd64-cross，缺失时在此补装
      # （仅本临时容器，不改共享镜像；AA64 在 arm64 容器内即原生目标，无需补）。
      if [ "'"$arch"'" = X64 ] && [ ! -f /usr/x86_64-linux-gnu/include/stdint.h ]; then
        echo "installing libc6-dev-amd64-cross (cross libc headers for stdint.h)"
        apt-get update -qq && apt-get install -y -qq libc6-dev-amd64-cross
      fi
      export WORKSPACE=/work/third_party/ntfs-3g/src
      export PACKAGES_PATH=/edk2
      export EDK_TOOLS_PATH=/edk2/BaseTools
      export CONF_PATH=/work/third_party/ntfs-3g/conf
      mkdir -p "$CONF_PATH"
      source /edk2/edksetup.sh --reconfig >/dev/null 2>&1
      build -a '"$arch"' -b RELEASE -t GCC -p uefi-driver.dsc \
        -D DRIVER_VERSION=1.3 -D COMMIT_INFO=0964af8
    ' 2>&1 | tee "$NTFS/.build-$arch.log" | tail -5
  # DSC 显式 OUTPUT_DIRECTORY=Build → 输出无平台名层级：Build/RELEASE_GCC/<ARCH>/...
  local efi="$NTFS/src/Build/RELEASE_GCC/$arch/uefi-driver/uefi_driver/OUTPUT/ntfs.efi"
  [[ -f "$efi" ]] || { echo "ERROR: expected output missing: $efi" >&2; exit 1; }
  local outname
  case "$arch" in X64) outname=ntfs_x64.efi;; AARCH64) outname=ntfs_aa64.efi;; esac
  cp "$efi" "$NTFS/out/$outname"
  echo "out: $NTFS/out/$outname"
}

if [[ "$ARCH" == X64 || "$ARCH" == all ]]; then build_one X64; fi
if [[ "$ARCH" == AARCH64 || "$ARCH" == all ]]; then build_one AARCH64; fi

# PE 机器类型校验（T14-1）：0x8664=AMD64，0xAA64=ARM64
python3 - "$NTFS/out" <<'EOF'
import struct, sys, os
out = sys.argv[1]
expect = {"ntfs_x64.efi": 0x8664, "ntfs_aa64.efi": 0xAA64}
for name, machine in expect.items():
    p = os.path.join(out, name)
    if not os.path.exists(p):
        continue
    data = open(p, "rb").read()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe_off:pe_off+4] == b"PE\0\0", f"{name}: bad PE signature"
    got = struct.unpack_from("<H", data, pe_off+4)[0]
    assert got == machine, f"{name}: machine 0x{got:04X} != 0x{machine:04X}"
    print(f"PE check OK: {name} machine=0x{got:04X}")
EOF

# SHA256 清单（仅列当前存在的产物）
( cd "$NTFS/out"
  files=()
  for f in ntfs_x64.efi ntfs_aa64.efi; do [[ -f "$f" ]] && files+=("$f"); done
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "${files[@]}"
  else shasum -a 256 "${files[@]}"; fi ) > "$NTFS/out/SHA256SUMS"
cat "$NTFS/out/SHA256SUMS"
echo "done."