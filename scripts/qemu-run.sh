#!/usr/bin/env bash
# 原生 QEMU 运行器（macOS）。USB 挂盘走真实可移动介质 fallback 路径（design §9）。
# 用法: qemu-run.sh --arch x64|aa64 --image <img> [--timeout SEC] [--keys <file>]
#                   [--expect <patterns-file>] [--extra "qemu args"]
# serial 日志: <img>.serial.log；--expect 文件每行一个 grep -E 模式，全部命中=PASS。
# 固件: Homebrew QEMU 自带 edk2 blobs（x64=OVMF 分体 pflash；aa64=ArmVirtQemu）。
set -euo pipefail
ARCH=x64; IMG=""; TIMEOUT=180; KEYS=""; EXPECT=""; EXTRA=""
while [[ $# -gt 0 ]]; do case "$1" in
  --arch) ARCH="$2"; shift 2;; --image) IMG="$2"; shift 2;;
  --timeout) TIMEOUT="$2"; shift 2;; --keys) KEYS="$2"; shift 2;;
  --expect) EXPECT="$2"; shift 2;; --extra) EXTRA="$2"; shift 2;;
  *) echo "unknown arg: $1" >&2; exit 2;; esac; done
[[ -n "$IMG" && -f "$IMG" ]] || { echo "image required" >&2; exit 2; }
FW="${QEMU_FW_DIR:-/opt/homebrew/share/qemu}"
LOG="${IMG%.img}.serial.log"
: > "$LOG"
WORK=$(mktemp -d)
QMP="$WORK/qmp.sock"
cleanup() { [[ -n "${QPID:-}" ]] && kill "$QPID" 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

if [[ "$ARCH" == x64 ]]; then
  cp "$FW/edk2-i386-vars.fd" "$WORK/vars.fd"
  set -- qemu-system-x86_64 -machine q35 -m 2G -accel tcg,thread=multi \
    -drive if=pflash,format=raw,readonly=on,file="$FW/edk2-x86_64-code.fd" \
    -drive if=pflash,format=raw,file="$WORK/vars.fd"
else
  # aa64 virt 无默认键盘（S07 实证），补 usb-kbd；x64 q35 自带 ps2-kbd 勿加（双份事件）
  set -- qemu-system-aarch64 -machine virt -cpu host -m 2G -accel hvf \
    -bios "$FW/edk2-aarch64-code.fd" -device usb-kbd
fi
set -- "$@" -drive id=stick,file="$IMG",format=raw,if=none \
  -device qemu-xhci -device usb-storage,drive=stick \
  -serial "file:$LOG" -qmp "unix:$QMP,server=on,wait=off" \
  -display none $EXTRA

"$@" & QPID=$!

if [[ -n "$KEYS" ]]; then
  python3 "$(dirname "$0")/qmp-keys.py" "$QMP" "$KEYS" &
fi

deadline=$(( $(date +%s) + TIMEOUT ))
ok=1
while (( $(date +%s) < deadline )); do
  if ! kill -0 "$QPID" 2>/dev/null; then break; fi
  if [[ -n "$EXPECT" ]]; then
    ok=1
    while IFS= read -r pat; do
      [[ -z "$pat" || "$pat" == \#* ]] && continue
      grep -Eq "$pat" "$LOG" || { ok=0; break; }
    done < "$EXPECT"
    (( ok )) && { echo "PASS: all expect patterns matched"; exit 0; }
  fi
  sleep 2
done
if [[ -n "$EXPECT" ]]; then
  echo "FAIL: timeout ${TIMEOUT}s; serial tail:" >&2
  tail -20 "$LOG" >&2
  exit 1
fi
echo "RUN COMPLETE (no expect file); serial: $LOG"
