/** @file
  TuiScreenLib 内部后端接缝（非冻结契约，仅本目录与宿主测试使用）。

  核心 TuiScreen.c 双环境共享，不 include 任何 UEFI 协议头；ConOut 访问经
  TUI_CONOUT 函数表在链接期注入：
    - UEFI 构建：TuiScreenUefi.c 提供 TuiScreenBackend()，包装 gST->ConOut；
    - 宿主测试：tests/ 提供 TuiScreenBackend()，返回记录调用序列的 mock ConOut。

  模式选择护栏为具名常量（配置优先纪律；EasyEfiPkg.dec 为 leader 独占，
  不新增 PCD，常量集中于此便于调整）。
**/
#ifndef TUI_SCREEN_INTERNAL_H
#define TUI_SCREEN_INTERNAL_H

#include "Contract/EfiCompat.h"
#include "Contract/TuiScreen.h"

/* QueryMode 结果护栏：超出视为非法模式跳过（防分配溢出） */
#define TUI_SCREEN_MAX_COLS       512
#define TUI_SCREEN_MAX_ROWS       256
/* 保底模式（UEFI 规范保证 mode 0 = 80x25；枚举全失败时不 SetMode，按此尺寸建缓冲） */
#define TUI_SCREEN_FALLBACK_COLS  80
#define TUI_SCREEN_FALLBACK_ROWS  25
/* 模式枚举护栏：候选模式上限 / 探测的模式号上限（防 MaxMode 异常大） */
#define TUI_SCREEN_MAX_MODES      64
#define TUI_SCREEN_MODE_PROBE_LIMIT 256

typedef struct {
  UINTN      MaxMode;        /* 可探测的模式槽位数（ConOut->Mode->MaxMode） */
  INTN       CurrentMode;    /* 进入前模式（ConOut->Mode->Mode）；-1 = 无有效模式 */
  EFI_STATUS (*QueryMode) (UINTN Mode, UINTN *Cols, UINTN *Rows);
  EFI_STATUS (*SetMode) (UINTN Mode);
  EFI_STATUS (*SetCursorPosition) (UINTN Col, UINTN Row);
  EFI_STATUS (*SetAttribute) (UINTN Attr);
  EFI_STATUS (*OutputString) (CONST CHAR16 *Str);
} TUI_CONOUT;

/* 链接期注入点：UEFI = TuiScreenUefi.c；宿主 = 测试 mock。TuiInit 时调用。 */
TUI_CONOUT *TuiScreenBackend (VOID);

/* 内部只读探针（宿主黄金快照用；越界/未初始化返回 NULL） */
CONST TUI_CELL *TuiScreenPeekCell (UINTN Row, UINTN Col);

#endif /* TUI_SCREEN_INTERNAL_H */
