/** @file
  TuiScreenLib UEFI 后端（纯 glue）— 包装 gST->ConOut 为 TUI_CONOUT 函数表。
  仅 UEFI 构建编译（.inf Sources）；宿主测试以 tests/MockConOut.c 替代本文件。
  gST 由 UefiBootServicesTableLib 构造器注入，先于本库使用。
**/
#include <Uefi.h>
#include <Protocol/SimpleTextOut.h>
#include <Library/UefiBootServicesTableLib.h>
#include "TuiScreenInternal.h"

STATIC EFI_STATUS UefiQueryMode (UINTN Mode, UINTN *Cols, UINTN *Rows) {
  return gST->ConOut->QueryMode (gST->ConOut, Mode, Cols, Rows);
}

STATIC EFI_STATUS UefiSetMode (UINTN Mode) {
  return gST->ConOut->SetMode (gST->ConOut, Mode);
}

STATIC EFI_STATUS UefiSetCursorPosition (UINTN Col, UINTN Row) {
  return gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row);
}

STATIC EFI_STATUS UefiSetAttribute (UINTN Attr) {
  return gST->ConOut->SetAttribute (gST->ConOut, Attr);
}

STATIC EFI_STATUS UefiOutputString (CONST CHAR16 *Str) {
  /* 协议原型为非 CONST CHAR16*；缓冲为本库私有 mRun，去 const 安全 */
  return gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)Str);
}

TUI_CONOUT *TuiScreenBackend (VOID) {
  STATIC TUI_CONOUT Be;
  if (gST == NULL || gST->ConOut == NULL || gST->ConOut->Mode == NULL) {
    return NULL;
  }
  /* TuiInit 时读取，反映当前 ConOut 状态 */
  Be.MaxMode         = (UINTN)gST->ConOut->Mode->MaxMode;
  Be.CurrentMode     = (INTN)gST->ConOut->Mode->Mode;
  Be.QueryMode       = UefiQueryMode;
  Be.SetMode         = UefiSetMode;
  Be.SetCursorPosition = UefiSetCursorPosition;
  Be.SetAttribute    = UefiSetAttribute;
  Be.OutputString    = UefiOutputString;
  return &Be;
}
