#!/usr/bin/env python3
"""QMP 按键注入：配合 qemu-run.sh --keys。
keys 文件格式（每行）：<延迟秒> <key> [<key> ...]
key 名用 QEMU send-key 名（esc/ret/up/down/left/right/f1..f10/spc/字符本身）。
例:
  10 esc
  15 down ret
"""
import socket, json, sys, time, os

def qmp_cmd(sock, cmd, **args):
    sock.sendall((json.dumps({"execute": cmd, "arguments": args}) + "\n").encode())
    f = sock.makefile("rb")
    while True:
        line = f.readline()
        if not line:
            raise RuntimeError("QMP closed")
        msg = json.loads(line)
        if "return" in msg or "error" in msg:
            if "error" in msg:
                raise RuntimeError(msg["error"])
            return msg

def main():
    sock_path, keys_file = sys.argv[1], sys.argv[2]
    events = []
    for line in open(keys_file):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        events.append((float(parts[0]), parts[1:]))
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(60):
        try:
            s.connect(sock_path)
            break
        except OSError:
            time.sleep(0.5)
    else:
        raise RuntimeError("QMP socket not ready")
    f = s.makefile("rb")
    json.loads(f.readline())  # greeting
    qmp_cmd(s, "qmp_capabilities")
    start = time.time()
    for delay, keys in events:
        wait = delay - (time.time() - start)
        if wait > 0:
            time.sleep(wait)
        qmp_cmd(s, "send-key", keys=[{"type": "qcode", "data": k} for k in keys])
    s.close()

if __name__ == "__main__":
    main()
