/** @file
  S10 冒烟目标 .efi（TEST-ONLY）：打印标记 + 回显 LoadOptions + 改 ConOut 模式
  + 返回非零（EFI_ABORTED），供 ExecEfi 的状态捕获/控制台恢复断言。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include "Contract/EfiCompat.h"

#ifdef MDE_CPU_X64
#define ARCH_NAME L"x64"
#else
#define ARCH_NAME L"aa64"
#endif

EFI_STATUS EFIAPI UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_LOADED_IMAGE_PROTOCOL *Li = NULL;

  gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li);
  Print (L"[S10-TARGET] ran arch=%s\n", ARCH_NAME);
  if (Li != NULL && Li->LoadOptions != NULL && Li->LoadOptionsSize >= 2) {
    Print (L"[S10-TARGET] opts=%s\n", (CHAR16 *)Li->LoadOptions);
  }
  if (gST->ConOut->Mode->MaxMode > 1) {
    UINTN NewMode = (gST->ConOut->Mode->Mode == 0) ? 1 : 0;
    gST->ConOut->SetMode (gST->ConOut, NewMode);
    Print (L"[S10-TARGET] mode-changed\n");
  }
  return EFI_ABORTED;                      /* 非零返回 → ImageReturn 捕获断言 */
}