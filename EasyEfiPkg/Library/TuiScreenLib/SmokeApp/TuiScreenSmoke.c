/** @file
  S06 QEMU 冒烟应用：TuiInit → ASCII 画框 + 文本 → Flush → serial 标记。
  断言: [S06] mode=<cols>x<rows> PASS（expect-smoke.txt）。
  纯 UEFI glue：裸 L"" 允许（编码纪律 1）。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Contract/TuiScreen.h"
#include "Contract/TuiTheme.h"

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;
  UINTN      Rows, Cols, r, c;

  Status = TuiInit ();
  if (EFI_ERROR (Status)) {
    Print (L"[S06] TuiInit FAIL (0x%lx)\n", (UINT64)Status);
    return Status;
  }
  TuiGetSize (&Rows, &Cols);

  /* 整屏正常底色 */
  TuiFill (0, 0, Cols, Rows, ' ', TUI_ATTR_NORMAL);

  if (Rows >= 10 && Cols >= 40) {
    /* BoxDraw 风格 ASCII 画框（+-|），内缩 1 格，不触碰屏幕右下角 */
    for (c = 2; c + 2 < Cols; c++) {
      TuiPutCell (1, c, '-', TUI_ATTR_TITLE);
      TuiPutCell (Rows - 2, c, '-', TUI_ATTR_TITLE);
    }
    for (r = 2; r + 2 < Rows; r++) {
      TuiPutCell (r, 1, '|', TUI_ATTR_TITLE);
      TuiPutCell (r, Cols - 2, '|', TUI_ATTR_TITLE);
    }
    TuiPutCell (1, 1, '+', TUI_ATTR_TITLE);
    TuiPutCell (1, Cols - 2, '+', TUI_ATTR_TITLE);
    TuiPutCell (Rows - 2, 1, '+', TUI_ATTR_TITLE);
    TuiPutCell (Rows - 2, Cols - 2, '+', TUI_ATTR_TITLE);
    TuiPutText (1, 3, L" EasyEFI S06 ", TUI_ATTR_TITLE);
    TuiPutText (3, 3, L"TuiScreenLib ConOut backend", TUI_ATTR_NORMAL);
    TuiPutText (4, 3, L"ASCII box + dirty flush OK", TUI_ATTR_NORMAL);
  } else {
    TuiPutText (0, 0, L"EasyEFI S06", TUI_ATTR_TITLE);
  }

  Status = TuiFlush ();
  if (EFI_ERROR (Status)) {
    Print (L"[S06] TuiFlush FAIL (0x%lx)\n", (UINT64)Status);
    TuiShutdown ();
    return Status;
  }

  /* 标记打在框内安全位置（行 6 远离底行，避免 Print 换行滚动） */
  if (Rows >= 10 && Cols >= 40) {
    gST->ConOut->SetCursorPosition (gST->ConOut, 3, 6);
  }
  Print (L"[S06] mode=%dx%d PASS\n", (INT32)Cols, (INT32)Rows);

  TuiShutdown ();   /* 恢复进入前模式 */
  return EFI_SUCCESS;
}
