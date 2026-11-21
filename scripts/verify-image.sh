#!/usr/bin/env bash
# S16: 镜像布局校验（design §3.1）。容器内执行（sgdisk）。
# 用法: verify-image.sh <img>
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="${1:?usage: verify-image.sh <img>}"
docker run --rm -e IMG="$IMG" -v "$ROOT":/work -w /work easyefi-build bash -c '
set -e
T=$(sgdisk -p "$IMG")
echo "$T" | grep -q "Disk identifier (GUID)" || { echo "FAIL: no GPT"; exit 1; }
P1=$(echo "$T" | awk "/^ +1 +/ {print \$6}")
P2=$(echo "$T" | awk "/^ +2 +/ {print \$6}")
P3=$(echo "$T" | awk "/^ +3 +/ {print \$6}")
[[ "$P1" == "EF00" && "$P2" == "EF00" && "$P3" == "0700" ]] || { echo "FAIL: types $P1/$P2/$P3 (want EF00/EF00/0700)"; exit 1; }
S1=$(echo "$T" | awk "/^ +1 +/ {print \$2}")
[[ "$S1" == "2048" ]] || { echo "FAIL: P1 start $S1 (want 2048 = 1MiB aligned)"; exit 1; }
V=$(sgdisk -v "$IMG" 2>&1)
echo "$V" | grep -q "No problems found" || { echo "FAIL: $V"; exit 1; }
echo "VERIFY PASS: 3 partitions EF00/EF00/0700, 1MiB aligned, GPT valid"
'
