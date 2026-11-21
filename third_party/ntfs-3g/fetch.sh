#!/usr/bin/env bash
# S14: 拉取锁定 commit 的 pbatard/ntfs-3g 源码（codeload tarball + SHA256 校验）。
# 用法: fetch.sh [--force]
# 锁定记录: docs/pins/S14-ntfs3g.md（commit SHA 为权威；tarball 哈希为完整性校验）。
# 幂等：src/.pin 与锁定 SHA 一致则跳过；--force 强制重拉。
# 本脚本同时是 GPLv2 源码提供指引的机器可执行形式（design.md §8 书面要约）。
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIN_SHA=0964af83716d57a15bad94da53e8f38c4dc266c2
TARBALL_SHA256=4a25b19a13fbb3dc129aa613c380a7c64b339749e4ad2b423e0f9f48111fc366
URL="https://codeload.github.com/pbatard/ntfs-3g/tar.gz/$PIN_SHA"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1
if [[ $FORCE == 0 && -f "$DIR/src/.pin" ]] && grep -q "$PIN_SHA" "$DIR/src/.pin"; then
  echo "src already at pin $PIN_SHA (use --force to re-fetch)"
  exit 0
fi

if command -v sha256sum >/dev/null 2>&1; then SHA256=(sha256sum)
elif command -v shasum >/dev/null 2>&1; then SHA256=(shasum -a 256)
else echo "need sha256sum or shasum for integrity check" >&2; exit 1; fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "fetching $URL"
curl -sSL --retry 3 --retry-delay 5 --max-time 570 -o "$TMP/ntfs3g.tar.gz" "$URL"
echo "$TARBALL_SHA256  $TMP/ntfs3g.tar.gz" | "${SHA256[@]}" -c -

rm -rf "$DIR/src"
mkdir -p "$DIR/src"
tar -xzf "$TMP/ntfs3g.tar.gz" -C "$DIR/src" --strip-components=1
{
  echo "repo: https://github.com/pbatard/ntfs-3g"
  echo "branch: rebase-on-2026.9.28 (default)"
  echo "commit: $PIN_SHA"
  echo "tarball_sha256: $TARBALL_SHA256"
  echo "fetched_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$DIR/src/.pin"
echo "source ready: $DIR/src (pin $PIN_SHA)"