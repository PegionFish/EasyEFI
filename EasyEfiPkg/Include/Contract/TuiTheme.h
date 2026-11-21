/** @file
  TUI 主题常量与命令枚举（契约，S00 冻结，三界面强制一致）。
  属性值 = UEFI ConOut 文本属性（前景低 4 位 | 背景<<4）。
**/
#ifndef TUI_THEME_H
#define TUI_THEME_H

#include "EfiCompat.h"

/* 前景色（UEFI SimpleTextOutput） */
#define TUI_FG_BLACK        0x00
#define TUI_FG_BLUE         0x01
#define TUI_FG_GREEN        0x02
#define TUI_FG_CYAN         0x03
#define TUI_FG_RED          0x04
#define TUI_FG_MAGENTA      0x05
#define TUI_FG_BROWN        0x06
#define TUI_FG_LIGHTGRAY    0x07
#define TUI_FG_DARKGRAY     0x08
#define TUI_FG_LIGHTBLUE    0x09
#define TUI_FG_LIGHTGREEN   0x0A
#define TUI_FG_LIGHTCYAN    0x0B
#define TUI_FG_LIGHTRED     0x0C
#define TUI_FG_LIGHTMAGENTA 0x0D
#define TUI_FG_YELLOW       0x0E
#define TUI_FG_WHITE        0x0F
#define TUI_BG(C)           ((C) << 4)

/* 语义属性（design §5.5：高亮=选中，红=危险/错误，灰=不可用） */
#define TUI_ATTR_NORMAL     (TUI_FG_LIGHTGRAY | TUI_BG(TUI_FG_BLACK))
#define TUI_ATTR_SELECTED   (TUI_FG_BLACK     | TUI_BG(TUI_FG_LIGHTCYAN))
#define TUI_ATTR_DANGER     (TUI_FG_LIGHTRED  | TUI_BG(TUI_FG_BLACK))
#define TUI_ATTR_DISABLED   (TUI_FG_DARKGRAY  | TUI_BG(TUI_FG_BLACK))
#define TUI_ATTR_NONASCII   (TUI_FG_YELLOW    | TUI_BG(TUI_FG_BLACK))
#define TUI_ATTR_STATUSBAR  (TUI_FG_BLACK     | TUI_BG(TUI_FG_LIGHTGRAY))
#define TUI_ATTR_BANNER_ERR (TUI_FG_WHITE     | TUI_BG(TUI_FG_RED))
#define TUI_ATTR_TITLE      (TUI_FG_LIGHTCYAN | TUI_BG(TUI_FG_BLACK))
#define TUI_ATTR_KEYBAR     (TUI_FG_BLACK     | TUI_BG(TUI_FG_LIGHTGRAY))

/* 非 ASCII 占位符（design §7.3：单格黄色 ?，光标处状态栏显示 U+XXXX） */
#define TUI_NONASCII_CHAR   ((CHAR16)'?')

/* 抽象按键命令（TuiKeyToCommand 输出；CHAR 命令附带 UnicodeChar） */
typedef enum {
  CMD_NONE = 0,
  CMD_UP, CMD_DOWN, CMD_LEFT, CMD_RIGHT,
  CMD_PGUP, CMD_PGDN, CMD_HOME, CMD_END,
  CMD_ENTER, CMD_ESC, CMD_BACKSPACE, CMD_DELETE, CMD_TAB,
  CMD_F1, CMD_F2, CMD_F3, CMD_F4, CMD_F5,
  CMD_F6, CMD_F7, CMD_F8, CMD_F9, CMD_F10,
  CMD_CHAR            /* 可打印字符，见 TUI_KEY.UnicodeChar */
} TUI_COMMAND;

#endif
