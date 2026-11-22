#!/usr/bin/env python3
"""QEMU UI 截图：QMP screendump 抓 GOP 图形帧缓冲 → PNG。
用法: qemu-screenshot.py --arch aa64|x64 --image <img> --out <png>
        [--marker <serial标记>] [--keys <keyfile>] [--post-delay SEC] [--extra "qemu args"]
等待 serial 出现 marker（默认 EasyEFI）后截图；可选注入按键后再截（--post-delay 等待渲染）。
"""
import socket, json, sys, time, os, subprocess, argparse, tempfile, shutil

FW = os.environ.get("QEMU_FW_DIR", "/opt/homebrew/share/qemu")

def qmp(sock, cmd, **args):
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

def send_keys(sock, keyfile):
    start = time.time()
    for line in open(keyfile):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        delay = float(parts[0])
        wait = delay - (time.time() - start)
        if wait > 0:
            time.sleep(wait)
        qmp(sock, "send-key", keys=[{"type": "qcode", "data": k} for k in parts[1:]])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--marker", default="EasyEFI")
    ap.add_argument("--keys")
    ap.add_argument("--post-delay", type=float, default=3.0)
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--extra", default="")
    a = ap.parse_args()

    work = tempfile.mkdtemp()
    qmp_sock = os.path.join(work, "qmp.sock")
    serial = os.path.join(work, "serial.log")
    open(serial, "w").close()

    if a.arch == "x64":
        vars_fd = os.path.join(work, "vars.fd")
        shutil.copy(os.path.join(FW, "edk2-i386-vars.fd"), vars_fd)
        cmd = ["qemu-system-x86_64", "-machine", "q35", "-m", "2G", "-accel", "tcg,thread=multi",
               "-drive", f"if=pflash,format=raw,readonly=on,file={FW}/edk2-x86_64-code.fd",
               "-drive", f"if=pflash,format=raw,file={vars_fd}",
               "-device", "VGA"]
    else:
        cmd = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "host", "-m", "2G", "-accel", "hvf",
               "-bios", f"{FW}/edk2-aarch64-code.fd", "-device", "ramfb"]
    cmd += ["-drive", f"id=stick,file={a.image},format=raw,if=none",
            "-device", "qemu-xhci", "-device", "usb-storage,drive=stick"]
    if a.arch != "x64":
        cmd += ["-device", "usb-kbd"]   # aa64 virt 无默认键盘，须在 xhci 之后
    cmd += ["-serial", f"file:{serial}", "-qmp", f"unix:{qmp_sock},server=on,wait=off",
            "-display", "none"]
    if a.extra:
        cmd += a.extra.split()

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(60):
            try:
                s.connect(qmp_sock)
                break
            except OSError:
                time.sleep(0.5)
        else:
            raise RuntimeError("QMP socket not ready")
        f = s.makefile("rb")
        json.loads(f.readline())
        qmp(s, "qmp_capabilities")

        # 等待 marker
        deadline = time.time() + a.timeout
        found = False
        while time.time() < deadline:
            try:
                data = open(serial, "rb").read().decode("utf-8", "replace")
                if a.marker in data:
                    found = True
                    break
            except OSError:
                pass
            time.sleep(1)
        if not found:
            print(f"WARN: marker '{a.marker}' not seen in {a.timeout}s; dumping anyway", file=sys.stderr)
        time.sleep(2)  # 渲染稳定

        if a.keys:
            send_keys(s, a.keys)
            time.sleep(a.post_delay)

        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        qmp(s, "screendump", filename=os.path.abspath(a.out), format="png")
        print(f"saved {a.out} ({os.path.getsize(a.out)} bytes)")
        s.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(work, ignore_errors=True)

if __name__ == "__main__":
    main()
