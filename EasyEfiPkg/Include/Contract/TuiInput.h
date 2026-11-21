/** @file
  按键 Ops（契约，S00 冻结；S07 实现 UEFI 后端，S00 提供宿主 mock）。
  全键盘（design D7）：无鼠标/指针协议。
**/
#ifndef TUI_INPUT_H
#define TUI_INPUT_H

#include "EfiCompat.h"
#include "TuiTheme.h"

/* UEFI 扫描码（UEFI Spec 2.10 §12.2） */
#ifndef SCAN_UP
#define SCAN_UP       0x01
#endif
#ifndef SCAN_DOWN
#define SCAN_DOWN     0x02
#endif
#ifndef SCAN_RIGHT
#define SCAN_RIGHT    0x03
#endif
#ifndef SCAN_LEFT
#define SCAN_LEFT     0x04
#endif
#ifndef SCAN_HOME
#define SCAN_HOME     0x05
#endif
#ifndef SCAN_END
#define SCAN_END      0x06
#endif
#ifndef SCAN_INSERT
#define SCAN_INSERT   0x07
#endif
#ifndef SCAN_DELETE
#define SCAN_DELETE   0x08
#endif
#ifndef SCAN_PGUP
#define SCAN_PGUP     0x09
#endif
#ifndef SCAN_PGDN
#define SCAN_PGDN     0x0A
#endif
#ifndef SCAN_F1
#define SCAN_F1       0x0B
#endif
#ifndef SCAN_F2
#define SCAN_F2       0x0C
#endif
#ifndef SCAN_F3
#define SCAN_F3       0x0D
#endif
#ifndef SCAN_F4
#define SCAN_F4       0x0E
#endif
#ifndef SCAN_F5
#define SCAN_F5       0x0F
#endif
#ifndef SCAN_F6
#define SCAN_F6       0x10
#endif
#ifndef SCAN_F7
#define SCAN_F7       0x11
#endif
#ifndef SCAN_F8
#define SCAN_F8       0x12
#endif
#ifndef SCAN_F9
#define SCAN_F9       0x13
#endif
#ifndef SCAN_F10
#define SCAN_F10      0x14
#endif
#ifndef SCAN_ESC
#define SCAN_ESC      0x17
#endif

typedef struct {
  UINT16 ScanCode;      /* 0 = 可打印字符 */
  CHAR16 UnicodeChar;
} TUI_KEY;

EFI_STATUS  TuiWaitKey (TUI_KEY *Key);     /* 阻塞（事件等待） */
EFI_STATUS  TuiPollKey (TUI_KEY *Key);     /* 非阻塞；无键返回 EFI_NOT_READY */
TUI_COMMAND TuiKeyToCommand (CONST TUI_KEY *Key);

#endif
