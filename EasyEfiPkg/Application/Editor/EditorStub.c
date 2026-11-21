/** @file
  Editor STUB 应用（S00 脚手架）。打印架构标记后置 EASYEFI_RAN 返回。
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Protocol/Shell.h>
#include <Protocol/LoadedImage.h>

EFI_STATUS EFIAPI UefiMain (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_SHELL_PROTOCOL *Shell;
#if defined(MDE_CPU_X64)
  Print (L"[STUB] Editor x64\n");
#elif defined(MDE_CPU_AARCH64)
  Print (L"[STUB] Editor aarch64\n");
#endif
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiShellProtocolGuid, NULL, (VOID **)&Shell))) {
    Shell->SetEnv (L"EASYEFI_RAN", L"yes");
  }
  return EFI_SUCCESS;
}
