#!/bin/bash
# EasyEFI 一键写盘：双击运行，输入一次 sudo 密码即可
cd /Users/bob/EasyEFI || { echo "workspace not found"; exit 1; }
sudo /bin/bash /Users/bob/EasyEFI/scripts/flash-usb-macos.sh /dev/disk5
rc=$?
echo
echo "exit=$rc"
echo "日志: /Users/bob/EasyEFI/.flash-usb.log"
