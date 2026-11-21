/** @file
  屏幕缓冲 Ops（契约，S00 冻结；S06 实现 UEFI 后端，S00 提供宿主 mock）。
  单元格矩阵 + 脏区重绘；纯 ASCII 画框；模式取最大可用文本模式（保底 80x25）。
**/
#ifndef TUI_SCREEN_H
#define TUI_SCREEN_H

#include "EfiCompat.h"
#include "TuiTheme.h"

typedef struct {
  CHAR16 Char;
  UINT8  Attr;
} TUI_CELL;

EFI_STATUS TuiInit (VOID);                 /* QueryMode→SetMode→分配缓冲 */
EFI_STATUS TuiGetSize (UINTN *Rows, UINTN *Cols);
VOID       TuiPutCell (UINTN Row, UINTN Col, CHAR16 Ch, UINT8 Attr);  /* 越界钳制丢弃 */
VOID       TuiPutText (UINTN Row, UINTN Col, CONST CHAR16 *Text, UINT8 Attr);
VOID       TuiFill (UINTN Row, UINTN Col, UINTN W, UINTN H, CHAR16 Ch, UINT8 Attr);
VOID       TuiClear (VOID);
EFI_STATUS TuiFlush (VOID);                /* 只重绘脏单元格 */
EFI_STATUS TuiShutdown (VOID);             /* 恢复进入前模式 */

#endif
