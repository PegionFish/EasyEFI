/** @file
  按键 Ops（契约，S00 冻结；S07 实现 UEFI 后端，S00 提供宿主 mock）。
  全键盘（design D7）：无鼠标/指针协议。
**/
#ifndef TUI_INPUT_H
#define TUI_INPUT_H

#include "EfiCompat.h"
#include "TuiTheme.h"

/* UEFI 扫描码（UEFI Spec 2.10 §12.2） */
#define SCAN_UP       0x01
#define SCAN_DOWN     0x02
#define SCAN_RIGHT    0x03
#define SCAN_LEFT     0x04
#define SCAN_HOME     0x05
#define SCAN_END      0x06
#define SCAN_INSERT   0x07
#define SCAN_DELETE   0x08
#define SCAN_PGUP     0x09
#define SCAN_PGDN     0x0A
#define SCAN_F1       0x0B
#define SCAN_F2       0x0C
#define SCAN_F3       0x0D
#define SCAN_F4       0x0E
#define SCAN_F5       0x0F
#define SCAN_F6       0x10
#define SCAN_F7       0x11
#define SCAN_F8       0x12
#define SCAN_F9       0x13
#define SCAN_F10      0x14
#define SCAN_ESC      0x17

typedef struct {
  UINT16 ScanCode;      /* 0 = 可打印字符 */
  CHAR16 UnicodeChar;
} TUI_KEY;

EFI_STATUS  TuiWaitKey (TUI_KEY *Key);     /* 阻塞（事件等待） */
EFI_STATUS  TuiPollKey (TUI_KEY *Key);     /* 非阻塞；无键返回 EFI_NOT_READY */
TUI_COMMAND TuiKeyToCommand (CONST TUI_KEY *Key);

#endif
